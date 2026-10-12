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

// A slider's head is part 1, judged by a press or a miss. The parts after it are judged by
// following, in turn.
constexpr uint32_t HEAD_PART = 1;
constexpr uint32_t FIRST_FOLLOWED_PART = 2;

bool IsSameTime(const CircleNote& a, const CircleNote& b)
{
    return std::abs(a.Time - b.Time) <= CHORD_TOLERANCE_US;
}

// Only circles lock later notes, as in lazer. Sliders and spinners never do.
bool CanBlock(const CircleNote& note)
{
    return note.Type == NoteType::Circle;
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
    switch (static_cast<CircleAction>(action.Type))
    {
    case CircleAction::Press:
        Press(action.Index, position, events);
        break;
    case CircleAction::Release:
        pointer.Position = position;
        pointer.IsDown = false;
        break;
    case CircleAction::Move:
        pointer.Position = position;
        break;
    case CircleAction::Touch:
        pointer.Position = position;
        pointer.IsDown = true;
        break;
    }
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
            Judge(check.Note, rhythm::Judgment::Miss, 0, NoteState::Missed, check.Time, events);
        }
        break;
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
        if (note.Type == NoteType::Spinner || untilNote > windows.MehUs || IsResolved(i))
        {
            continue;
        }
        glm::vec2 head = note.GetStackedPosition();
        if (note.Type == NoteType::Circle)
        {
            if (untilNote < -windows.MehUs)
            {
                continue;
            }
        }
        else
        {
            if (at > note.EndTime || note.PathPointCount < 2)
            {
                continue;
            }
            head = GetPathPosition(*chart, note, 0.0f);
        }
        if (glm::distance(position, head) > radius)
        {
            continue;
        }

        // The last pending circle before this note that isn't its chord partner.
        std::optional<uint32_t> blocker;
        for (uint32_t j = firstActive; j < i; ++j)
        {
            if (CanBlock(notes[j]) && !IsSameTime(notes[j], note) && !IsResolved(j))
            {
                blocker = j;
            }
        }
        return Target{i, blocker && at < notes[*blocker].Time};
    }
    return std::nullopt;
}

void CircleJudge::Press(uint32_t pointerIndex, glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events)
{
    Pointer& pointer = GetPointer(pointerIndex);
    pointer.Position = position;
    pointer.IsDown = true;

    const std::optional<Target> target = FindTarget(position, time);
    // A blocked press is spent: nothing is judged.
    if (!target || target->IsBlocked)
    {
        return;
    }
    const CircleNote& note = chart->Notes[target->Note];

    MissCirclesBefore(target->Note, events);
    const int64_t errorUs = time - note.Time;
    if (note.Type == NoteType::Slider)
    {
        states[target->Note] = NoteState::Hit;
        JudgePart(target->Note, HEAD_PART, windows.Classify(errorUs), errorUs, true, time, events);
        JudgePassedParts(target->Note, position, events);
        FinishSlider(target->Note);
    }
    else
    {
        Judge(target->Note, windows.Classify(errorUs), errorUs, NoteState::Hit, time, events);
    }
}

// Striking a note misses every earlier pending circle it skipped (lazer's force miss). A chord
// partner wasn't skipped.
void CircleJudge::MissCirclesBefore(uint32_t note, std::vector<rhythm::JudgmentEvent>& events)
{
    const CircleNote& struck = chart->Notes[note];
    for (uint32_t i = firstActive; i < note; ++i)
    {
        const CircleNote& earlier = chart->Notes[i];
        if (earlier.Type == NoteType::Circle && states[i] == NoteState::Pending &&
            earlier.Time < struck.Time - CHORD_TOLERANCE_US)
        {
            Judge(i, rhythm::Judgment::Miss, 0, NoteState::Missed, time, events);
        }
    }
}

void CircleJudge::Judge(uint32_t note, rhythm::Judgment result, int64_t errorUs, NoteState state, SongTimeUs eventTime,
                        std::vector<rhythm::JudgmentEvent>& events)
{
    states[note] = state;
    rhythm::JudgmentEvent event;
    event.NoteIndex = note;
    event.Result = result;
    event.ErrorUs = result == rhythm::Judgment::Miss ? 0 : errorUs;
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
    for (uint32_t i = 0; i < chart.Notes.size(); ++i)
    {
        const CircleNote& note = chart.Notes[i];
        if (note.Type == NoteType::Spinner)
        {
            continue;
        }
        uint32_t pointer = 0;
        while (pointer < busyUntil.size() && busyUntil[pointer] >= note.Time)
        {
            ++pointer;
        }
        if (pointer == busyUntil.size())
        {
            busyUntil.push_back(INT64_MIN);
        }

        const bool isSlider = note.Type == NoteType::Slider;
        const glm::vec2 head = isSlider ? GetPathPosition(chart, note, 0.0f) : note.GetStackedPosition();
        actions.push_back(MakeAction(CircleAction::Press, pointer, head, note.Time));
        if (!isSlider)
        {
            busyUntil[pointer] = note.Time + AUTOPLAY_RELEASE_US;
            actions.push_back(MakeAction(CircleAction::Release, pointer, head, busyUntil[pointer]));
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
