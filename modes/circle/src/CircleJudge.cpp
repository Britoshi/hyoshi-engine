#include "circle/CircleJudge.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace hyoshi::circle
{

namespace
{

// A press looks no further ahead than this. Far wider than any Bad window (0.2 s at OD 0), so it
// only bounds the scan.
constexpr SongTimeUs SCAN_AHEAD_US = 1'000'000;

// Autoplay lets go this long after a circle, and moves the pointer along a slider this often.
constexpr SongTimeUs AUTOPLAY_RELEASE_US = 30'000;
constexpr SongTimeUs AUTOPLAY_MOVE_STEP_US = 16'000;
// Autoplay sweeps across a flick from this long before it to as long after, at this many times its
// speed, from at least this many radii away.
constexpr SongTimeUs AUTOPLAY_SWEEP_US = 30'000;
constexpr float AUTOPLAY_SWEEP_SPEED = 1.5f;
constexpr float AUTOPLAY_SWEEP_RADII = 1.5f;

// A slider's head is part 1, judged by a press or a miss. The parts after it are judged by
// following, in turn.
constexpr uint32_t HEAD_PART = 1;
constexpr uint32_t FIRST_FOLLOWED_PART = 2;

bool IsSameTime(const CircleNote& a, const CircleNote& b)
{
    return std::abs(a.Time - b.Time) <= CHORD_TOLERANCE_US;
}

// Only pressed circles lock later notes, as in lazer. Sliders, spinners, slides, and flicks never
// do: a finger sweeping over the playfield touches a lot of it.
bool CanBlock(const CircleNote& note)
{
    return note.Type == NoteType::Circle && note.Head == HeadKind::Press;
}

// Whether a pointer going from `from` to `to` comes within `radius` of `point`.
bool Crosses(glm::vec2 from, glm::vec2 to, glm::vec2 point, float radius)
{
    const glm::vec2 step = to - from;
    const float lengthSquared = glm::dot(step, step);
    const float along =
        lengthSquared > 0.0f ? std::clamp(glm::dot(point - from, step) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return glm::distance(from + (step * along), point) <= radius;
}

// Whether a head can be struck: a slider's needs a path, and to be under way.
bool CanStrike(const CircleNote& note, SongTimeUs at)
{
    return note.Type == NoteType::Circle ||
           (note.Type == NoteType::Slider && note.PathPointCount >= 2 && at <= note.EndTime);
}

} // namespace

rhythm::GameplayAction MakeAction(CircleAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time)
{
    rhythm::GameplayAction action;
    action.Time = time;
    action.Type = static_cast<uint16_t>(type);
    action.Index = static_cast<uint16_t>(pointer);
    action.X = static_cast<int32_t>(std::lround(position.x * ACTION_POSITION_SCALE));
    action.Y = static_cast<int32_t>(std::lround(position.y * ACTION_POSITION_SCALE));
    return action;
}

glm::vec2 GetActionPosition(const rhythm::GameplayAction& action)
{
    return {static_cast<float>(action.X) / ACTION_POSITION_SCALE, static_cast<float>(action.Y) / ACTION_POSITION_SCALE};
}

void CircleJudge::Reset(const CircleChart& circleChart, const std::optional<CircleWindows>& givenWindows)
{
    chart = &circleChart;
    windows = givenWindows.value_or(chart->Difficulty.GetWindows());
    radius = chart->Difficulty.GetRadius();
    const size_t count = chart->Notes.size();
    states.assign(count, NoteState::Pending);
    finished.assign(count, false);
    started.assign(count, false);
    following.assign(count, false);
    nextPart.assign(count, FIRST_FOLLOWED_PART);
    slideChecked.assign(count, false);
    pointers.clear();
    time = INT64_MIN;
    firstActive = 0;
    unfinishedNotes = 0;
    for (size_t i = 0; i < count; ++i)
    {
        // Spinners aren't judged, so they're finished from the start.
        if (chart->Notes[i].Type == NoteType::Spinner)
        {
            finished[i] = true;
        }
        else
        {
            ++unfinishedNotes;
        }
    }
}

void CircleJudge::Advance(SongTimeUs target, std::vector<rhythm::JudgmentEvent>& events)
{
    if (target <= time)
    {
        return;
    }
    while (const std::optional<Check> check = FindNextCheck(target))
    {
        RunCheck(*check, events);
        AdvanceFirstActive();
    }
    time = target;
    AdvanceFirstActive();
}

void CircleJudge::ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events)
{
    Advance(action.Time, events);
    const glm::vec2 position = GetActionPosition(action);
    Pointer& pointer = GetPointer(action.Index);
    const auto type = static_cast<CircleAction>(action.Type);
    if (type == CircleAction::Release)
    {
        pointer.Position = position;
        pointer.IsDown = false;
        // A finger lifted is gone; the next to land is another.
        pointer.Track.clear();
        UpdateFollowingAll();
        return;
    }
    const glm::vec2 from = pointer.Track.empty() ? position : pointer.Position;
    pointer.Position = position;
    AddSample(pointer, time);
    // A press that strikes or is blocked is spent on that.
    bool isSpent = false;
    if (type == CircleAction::Press || type == CircleAction::Touch)
    {
        pointer.IsDown = true;
        isSpent = type == CircleAction::Press && Press(position, events);
    }
    if (pointer.IsDown && !isSpent)
    {
        Slide(position, events);
    }
    Flick(pointer, from, events);
    UpdateFollowingAll();
}

std::optional<SongTimeUs> CircleJudge::GetPressTarget(glm::vec2 position, SongTimeUs at) const
{
    const std::optional<Target> target = FindTarget(position, std::max(at, time));
    if (!target || target->IsBlocked)
    {
        return std::nullopt;
    }
    return chart->Notes[target->Note].Time;
}

// The earliest check due by `limit`: by time, then by note. Every check of a note comes at or
// after its time, so the scan stops at the first note after the limit.
std::optional<CircleJudge::Check> CircleJudge::FindNextCheck(SongTimeUs limit) const
{
    const std::vector<CircleNote>& notes = chart->Notes;
    std::optional<Check> next;
    const auto consider = [&](SongTimeUs at, uint32_t note, CheckType type)
    {
        if (at <= limit && (!next || at < next->Time))
        {
            next = Check{at, note, type};
        }
    };
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time <= limit; ++i)
    {
        const CircleNote& note = notes[i];
        if (finished[i])
        {
            continue;
        }
        if (!IsResolved(i))
        {
            consider(note.Time + windows.MehUs + 1, i, CheckType::Miss);
            if (note.Head == HeadKind::Slide && !slideChecked[i])
            {
                consider(note.Time, i, CheckType::SlideTime);
            }
        }
        if (note.Type != NoteType::Slider)
        {
            continue;
        }
        if (!started[i])
        {
            consider(note.Time, i, CheckType::SliderStart);
        }
        // The parts after the head wait for it to be judged, as lazer's do.
        else if (const SliderPart* part = GetSliderPart(*chart, i, nextPart[i]); part != nullptr && IsResolved(i))
        {
            consider(part->JudgeTime, i, CheckType::SliderPart);
        }
    }
    return next;
}

void CircleJudge::RunCheck(const Check& check, std::vector<rhythm::JudgmentEvent>& events)
{
    time = std::max(time, check.Time);
    switch (check.Type)
    {
    case CheckType::Miss:
        // Stamped with the moment it became a miss, which doesn't depend on when it was noticed.
        if (chart->Notes[check.Note].Type == NoteType::Slider)
        {
            states[check.Note] = NoteState::Missed;
            JudgePart(check.Note, HEAD_PART, rhythm::Judgment::Miss, 0, true, check.Time, events);
            FinishSlider(check.Note);
        }
        else
        {
            Judge(check.Note, rhythm::Judgment::Miss, 0, true, NoteState::Missed, check.Time, events);
        }
        break;
    case CheckType::SlideTime:
    {
        slideChecked[check.Note] = true;
        const CircleNote& note = chart->Notes[check.Note];
        if (!CanStrike(note, time))
        {
            break;
        }
        const glm::vec2 head = GetHeadPosition(*chart, note);
        const auto pointer =
            std::find_if(pointers.begin(), pointers.end(), [&](const Pointer& candidate)
                         { return candidate.IsDown && glm::distance(candidate.Position, head) <= radius; });
        if (pointer != pointers.end())
        {
            Strike(check.Note, pointer->Position, rhythm::Judgment::Great, 0, false, events);
        }
        break;
    }
    case CheckType::SliderStart:
        started[check.Note] = true;
        UpdateFollowing(check.Note, check.Time);
        break;
    case CheckType::SliderPart:
    {
        // A part held back for the head's miss is judged by following now.
        const uint32_t part = nextPart[check.Note]++;
        const SliderPart& sliderPart = *GetSliderPart(*chart, check.Note, part);
        UpdateFollowing(check.Note, time);
        JudgePart(check.Note, part, following[check.Note] ? rhythm::Judgment::Great : rhythm::Judgment::Miss, 0, false,
                  std::max(sliderPart.Time, time), events);
        FinishSlider(check.Note);
        break;
    }
    }
}

std::optional<CircleJudge::Target> CircleJudge::FindTarget(glm::vec2 position, SongTimeUs at) const
{
    const std::vector<CircleNote>& notes = chart->Notes;
    for (uint32_t i = firstActive; i < notes.size(); ++i)
    {
        const CircleNote& note = notes[i];
        const SongTimeUs untilNote = note.Time - at;
        // Time order: nothing after the first note beyond the horizon can match.
        if (untilNote > SCAN_AHEAD_US)
        {
            break;
        }
        if (note.Type == NoteType::Spinner || note.Head != HeadKind::Press || untilNote > windows.MehUs ||
            IsResolved(i))
        {
            continue;
        }
        // A circle is struck within its window; a slider's head is missed at the end of its window,
        // and can't be struck once the slider ends.
        if ((note.Type == NoteType::Circle && untilNote < -windows.MehUs) || !CanStrike(note, at) ||
            glm::distance(position, GetHeadPosition(*chart, note)) > radius)
        {
            continue;
        }
        return Target{i, IsBlocked(i, at)};
    }
    return std::nullopt;
}

bool CircleJudge::IsBlocked(uint32_t note, SongTimeUs at) const
{
    // The last pending circle before this note that isn't its chord partner.
    const std::vector<CircleNote>& notes = chart->Notes;
    std::optional<uint32_t> blocker;
    for (uint32_t j = firstActive; j < note; ++j)
    {
        if (CanBlock(notes[j]) && !IsSameTime(notes[j], notes[note]) && !IsResolved(j))
        {
            blocker = j;
        }
    }
    return blocker && at < notes[*blocker].Time;
}

bool CircleJudge::Press(glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events)
{
    const std::optional<Target> target = FindTarget(position, time);
    if (!target)
    {
        return false;
    }
    // A blocked press is spent: nothing is judged.
    if (!target->IsBlocked)
    {
        const int64_t errorUs = time - chart->Notes[target->Note].Time;
        Strike(target->Note, position, windows.Classify(errorUs), errorUs, true, events);
    }
    return true;
}

// Slides are struck in time order, every one the pointer is on.
void CircleJudge::Slide(glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events)
{
    const std::vector<CircleNote>& notes = chart->Notes;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time <= time; ++i)
    {
        const CircleNote& note = notes[i];
        if (note.Head == HeadKind::Slide && !IsResolved(i) && time - note.Time <= windows.MehUs &&
            CanStrike(note, time) && glm::distance(position, GetHeadPosition(*chart, note)) <= radius)
        {
            Strike(i, position, rhythm::Judgment::Great, 0, false, events);
        }
    }
}

// Flicks are struck in time order, every one the pointer crossed fast enough (on the way from its
// last place, so however far it went since).
void CircleJudge::Flick(const Pointer& pointer, glm::vec2 from, std::vector<rhythm::JudgmentEvent>& events)
{
    const float speed = GetSpeed(pointer);
    if (speed <= 0.0f)
    {
        return;
    }
    const std::vector<CircleNote>& notes = chart->Notes;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time - time <= windows.MehUs; ++i)
    {
        const CircleNote& note = notes[i];
        if (note.Head == HeadKind::Flick && !IsResolved(i) && time - note.Time <= windows.MehUs &&
            CanStrike(note, time) && speed >= GetFlickSpeed(*chart, note) &&
            Crosses(from, pointer.Position, GetHeadPosition(*chart, note), radius) && !IsBlocked(i, time))
        {
            Strike(i, pointer.Position, rhythm::Judgment::Great, 0, false, events);
        }
    }
}

void CircleJudge::AddSample(Pointer& pointer, SongTimeUs at)
{
    std::vector<Sample>& track = pointer.Track;
    track.push_back({at, pointer.Position});
    // Where it was at the window's start is its last sample by then.
    const SongTimeUs start = at - FLICK_SPEED_WINDOW_US;
    size_t stale = 0;
    while (stale + 1 < track.size() && track[stale + 1].Time <= start)
    {
        ++stale;
    }
    track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(stale));
}

float CircleJudge::GetSpeed(const Pointer& pointer)
{
    const std::vector<Sample>& track = pointer.Track;
    if (track.size() < 2)
    {
        return 0.0f;
    }
    // From where it was at the window's start, or where it was first tracked if that's later.
    const Sample& now = track.back();
    const SongTimeUs span = std::min(now.Time - track.front().Time, FLICK_SPEED_WINDOW_US);
    if (span < FLICK_SPEED_WINDOW_US / 2)
    {
        return 0.0f;
    }
    return glm::distance(now.Position, track.front().Position) / (static_cast<float>(span) / 1.0e6f);
}

void CircleJudge::Strike(uint32_t note, glm::vec2 position, rhythm::Judgment result, int64_t errorUs, bool isTimed,
                         std::vector<rhythm::JudgmentEvent>& events)
{
    MissCirclesBefore(note, events);
    if (chart->Notes[note].Type == NoteType::Slider)
    {
        states[note] = NoteState::Hit;
        JudgePart(note, HEAD_PART, result, errorUs, isTimed, time, events);
        JudgePassedParts(note, position, events);
        FinishSlider(note);
        return;
    }
    Judge(note, result, errorUs, isTimed, NoteState::Hit, time, events);
}

// Striking a note misses every earlier pending circle it skipped (lazer's force miss): the ones
// that can block, pressed ones. A chord partner wasn't skipped, nor is a slide or a flick, which
// keeps its window (a slide's may not have opened yet).
void CircleJudge::MissCirclesBefore(uint32_t note, std::vector<rhythm::JudgmentEvent>& events)
{
    const CircleNote& struck = chart->Notes[note];
    for (uint32_t i = firstActive; i < note; ++i)
    {
        const CircleNote& earlier = chart->Notes[i];
        if (CanBlock(earlier) && states[i] == NoteState::Pending && earlier.Time < struck.Time - CHORD_TOLERANCE_US)
        {
            Judge(i, rhythm::Judgment::Miss, 0, true, NoteState::Missed, time, events);
        }
    }
}

void CircleJudge::Judge(uint32_t note, rhythm::Judgment result, int64_t errorUs, bool isTimed, NoteState state,
                        SongTimeUs eventTime, std::vector<rhythm::JudgmentEvent>& events)
{
    states[note] = state;
    rhythm::JudgmentEvent event;
    event.NoteIndex = note;
    event.Result = result;
    event.ErrorUs = result == rhythm::Judgment::Miss ? 0 : errorUs;
    event.IsTimed = isTimed;
    event.Time = eventTime;
    events.push_back(event);
    Finish(note);
}

void CircleJudge::JudgePart(uint32_t note, uint32_t part, rhythm::Judgment result, int64_t errorUs, bool isTimed,
                            SongTimeUs eventTime, std::vector<rhythm::JudgmentEvent>& events)
{
    rhythm::JudgmentEvent event;
    event.NoteIndex = note;
    event.Part = part;
    event.Result = result;
    event.ErrorUs = result == rhythm::Judgment::Miss ? 0 : errorUs;
    event.IsTimed = isTimed;
    event.Time = eventTime;
    events.push_back(event);
}

// lazer's SliderInputManager.PostProcessHeadJudgement: a head struck late judges the parts the
// ball has already passed, in order, by where the press was: hit while within the follow area of
// each, and once one is out of reach, it and the rest are missed.
void CircleJudge::JudgePassedParts(uint32_t note, glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events)
{
    const CircleNote& slider = chart->Notes[note];
    const float reach = radius * FOLLOW_RADIUS_SCALE;
    bool isInReach = true;
    for (const SliderPart* part = GetSliderPart(*chart, note, nextPart[note]);
         part != nullptr && part->JudgeTime <= time; part = GetSliderPart(*chart, note, nextPart[note]))
    {
        isInReach = isInReach && glm::distance(position, GetPathPosition(*chart, slider, part->Progress)) <= reach;
        JudgePart(note, nextPart[note]++, isInReach ? rhythm::Judgment::Great : rhythm::Judgment::Miss, 0, false, time,
                  events);
    }
}

// Once its head and every other part are judged. The slider itself has no result of its own.
void CircleJudge::FinishSlider(uint32_t note)
{
    if (IsResolved(note) && nextPart[note] > chart->Notes[note].PartCount)
    {
        Finish(note);
    }
}

bool CircleJudge::IsFollowedAt(uint32_t note, SongTimeUs at) const
{
    const CircleNote& slider = chart->Notes[note];
    if (at < slider.Time || at > slider.EndTime)
    {
        return false;
    }
    const glm::vec2 ball = GetBallPosition(*chart, slider, at);
    const float reach = following[note] ? radius * FOLLOW_RADIUS_SCALE : radius;
    return std::any_of(pointers.begin(), pointers.end(), [&](const Pointer& pointer)
                       { return pointer.IsDown && glm::distance(pointer.Position, ball) <= reach; });
}

void CircleJudge::UpdateFollowing(uint32_t note, SongTimeUs at)
{
    following[note] = started[note] && IsFollowedAt(note, at);
}

void CircleJudge::UpdateFollowingAll()
{
    const std::vector<CircleNote>& notes = chart->Notes;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time <= time; ++i)
    {
        if (notes[i].Type == NoteType::Slider && started[i])
        {
            UpdateFollowing(i, time);
        }
    }
}

void CircleJudge::Finish(uint32_t note)
{
    if (!finished[note])
    {
        finished[note] = true;
        --unfinishedNotes;
    }
}

// Everything before firstActive is finished and past its window, so no press can reach it.
void CircleJudge::AdvanceFirstActive()
{
    const std::vector<CircleNote>& notes = chart->Notes;
    while (firstActive < notes.size())
    {
        const CircleNote& note = notes[firstActive];
        if (time <= std::max(note.EndTime, note.Time) + windows.MehUs || !finished[firstActive])
        {
            break;
        }
        ++firstActive;
    }
}

CircleJudge::Pointer& CircleJudge::GetPointer(uint32_t pointer)
{
    if (pointer >= pointers.size())
    {
        pointers.resize(pointer + 1);
    }
    return pointers[pointer];
}

std::vector<rhythm::GameplayAction> MakeAutoplayActions(const CircleChart& chart)
{
    std::vector<rhythm::GameplayAction> actions;
    // When each pointer lets go. It can take a note after that.
    std::vector<SongTimeUs> busyUntil;
    const float radius = chart.Difficulty.GetRadius();
    for (uint32_t i = 0; i < chart.Notes.size(); ++i)
    {
        const CircleNote& note = chart.Notes[i];
        if (note.Type == NoteType::Spinner)
        {
            continue;
        }
        const bool isFlick = note.Head == HeadKind::Flick;
        const SongTimeUs start = note.Time - (isFlick ? AUTOPLAY_SWEEP_US : 0);
        uint32_t pointer = 0;
        while (pointer < busyUntil.size() && busyUntil[pointer] >= start)
        {
            ++pointer;
        }
        if (pointer == busyUntil.size())
        {
            busyUntil.push_back(INT64_MIN);
        }

        const bool isSlider = note.Type == NoteType::Slider;
        const glm::vec2 head = GetHeadPosition(chart, note);
        glm::vec2 last = head;
        if (isFlick)
        {
            // Across it the way from the note before (or rightward), fast enough, and starting
            // outside it, so it's struck on time.
            const glm::vec2 before = i > 0 ? GetHeadPosition(chart, chart.Notes[i - 1]) : head;
            const glm::vec2 heading =
                glm::distance(before, head) > 1.0f ? glm::normalize(head - before) : glm::vec2{1.0f, 0.0f};
            const float reach = std::max(GetFlickSpeed(chart, note) * AUTOPLAY_SWEEP_SPEED *
                                             (static_cast<float>(AUTOPLAY_SWEEP_US) / 1.0e6f),
                                         radius * AUTOPLAY_SWEEP_RADII);
            actions.push_back(MakeAction(CircleAction::Move, pointer, head - (heading * reach), start));
            actions.push_back(MakeAction(CircleAction::Move, pointer, head, note.Time));
            last = head + (heading * reach);
            if (isSlider)
            {
                actions.push_back(MakeAction(CircleAction::Touch, pointer, head, note.Time));
            }
        }
        else
        {
            // A slide is struck by a pointer coming down on it: touching, not pressing, so as not to
            // strike another note there.
            actions.push_back(MakeAction(note.Head == HeadKind::Slide ? CircleAction::Touch : CircleAction::Press,
                                         pointer, head, note.Time));
        }
        if (!isSlider)
        {
            busyUntil[pointer] = note.Time + (isFlick ? AUTOPLAY_SWEEP_US : AUTOPLAY_RELEASE_US);
            if (isFlick)
            {
                actions.push_back(MakeAction(CircleAction::Move, pointer, last, busyUntil[pointer]));
            }
            // Up, so a flick's sweep starts afresh.
            actions.push_back(MakeAction(CircleAction::Release, pointer, last, busyUntil[pointer]));
            continue;
        }
        // Along with the ball, and onto it whenever a part is judged.
        std::vector<SongTimeUs> moves;
        for (SongTimeUs t = note.Time + AUTOPLAY_MOVE_STEP_US; t < note.EndTime; t += AUTOPLAY_MOVE_STEP_US)
        {
            moves.push_back(t);
        }
        for (uint32_t part = FIRST_FOLLOWED_PART; part <= note.PartCount; ++part)
        {
            moves.push_back(GetSliderPart(chart, i, part)->JudgeTime);
        }
        std::sort(moves.begin(), moves.end());
        moves.erase(std::unique(moves.begin(), moves.end()), moves.end());
        for (const SongTimeUs t : moves)
        {
            actions.push_back(MakeAction(CircleAction::Move, pointer, GetBallPosition(chart, note, t), t));
        }
        busyUntil[pointer] = note.EndTime;
        actions.push_back(
            MakeAction(CircleAction::Release, pointer, GetBallPosition(chart, note, note.EndTime), note.EndTime));
    }
    std::stable_sort(actions.begin(), actions.end(),
                     [](const rhythm::GameplayAction& a, const rhythm::GameplayAction& b) { return a.Time < b.Time; });
    return actions;
}

} // namespace hyoshi::circle
