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

void CircleJudge::Reset(const CircleChart& circleChart)
{
    chart = &circleChart;
    windows = chart->Difficulty.GetWindows();
    radius = chart->Difficulty.GetRadius();
    states.assign(chart->Notes.size(), NoteState::Pending);
    finished.assign(chart->Notes.size(), false);
    pointers.clear();
    time = INT64_MIN;
    firstActive = 0;
    unfinishedNotes = 0;
    for (size_t i = 0; i < chart->Notes.size(); ++i)
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
    time = target;
    const std::vector<CircleNote>& notes = chart->Notes;

    // Misses, in time order: the first note still inside its window ends the walk.
    for (uint32_t i = firstActive; i < notes.size(); ++i)
    {
        if (time - notes[i].Time <= windows.MehUs)
        {
            break;
        }
        if (states[i] == NoteState::Pending && notes[i].Type != NoteType::Spinner)
        {
            // Stamped with the moment it became a miss, which doesn't depend on when it was noticed.
            Judge(i, rhythm::Judgment::Miss, 0, NoteState::Missed, notes[i].Time + windows.MehUs + 1, events);
        }
    }

    // A slider is let go once it ends.
    for (Pointer& pointer : pointers)
    {
        if (pointer.HeldSlider && time > notes[*pointer.HeldSlider].EndTime)
        {
            states[*pointer.HeldSlider] = NoteState::Hit;
            pointer.HeldSlider.reset();
        }
    }

    // Everything before firstActive is judged and past its window, so no press can reach it.
    while (firstActive < notes.size())
    {
        const CircleNote& note = notes[firstActive];
        const bool isPast = time > std::max(note.EndTime, note.Time) + windows.MehUs;
        if (!isPast || (note.Type != NoteType::Spinner && !IsResolved(firstActive)))
        {
            break;
        }
        ++firstActive;
    }
}

void CircleJudge::ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events)
{
    Advance(action.Time, events);
    const glm::vec2 position = GetActionPosition(action);
    switch (static_cast<CircleAction>(action.Type))
    {
    case CircleAction::Press:
        Press(action.Index, position, events);
        break;
    case CircleAction::Release:
        GetPointer(action.Index).Position = position;
        Release(action.Index);
        break;
    case CircleAction::Move:
        GetPointer(action.Index).Position = position;
        break;
    }
}

std::optional<CircleJudge::Target> CircleJudge::FindTarget(glm::vec2 position, bool canTakeSlider) const
{
    const std::vector<CircleNote>& notes = chart->Notes;
    for (uint32_t i = firstActive; i < notes.size(); ++i)
    {
        const CircleNote& note = notes[i];
        const SongTimeUs untilNote = note.Time - time;
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
            if (!canTakeSlider || time > note.EndTime || note.PathPointCount < 2)
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
        return Target{i, blocker && time < notes[*blocker].Time};
    }
    return std::nullopt;
}

void CircleJudge::Press(uint32_t pointerIndex, glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events)
{
    Pointer& pointer = GetPointer(pointerIndex);
    pointer.Position = position;
    pointer.IsDown = true;

    // A pointer follows one slider at a time.
    const std::optional<Target> target = FindTarget(position, !pointer.HeldSlider);
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
        Judge(target->Note, rhythm::Judgment::Great, errorUs, NoteState::Held, time, events);
        pointer.HeldSlider = target->Note;
    }
    else
    {
        Judge(target->Note, windows.Classify(errorUs), errorUs, NoteState::Hit, time, events);
    }
}

void CircleJudge::Release(uint32_t pointerIndex)
{
    Pointer& pointer = GetPointer(pointerIndex);
    pointer.IsDown = false;
    if (pointer.HeldSlider)
    {
        states[*pointer.HeldSlider] = NoteState::Hit;
        pointer.HeldSlider.reset();
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

void CircleJudge::Finish(uint32_t note)
{
    if (!finished[note])
    {
        finished[note] = true;
        --unfinishedNotes;
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
    struct Strike
    {
        uint32_t Note;
        uint32_t Pointer;
    };
    std::vector<Strike> strikes;
    for (uint32_t i = 0; i < chart.Notes.size(); ++i)
    {
        if (chart.Notes[i].Type != NoteType::Spinner)
        {
            strikes.push_back({i, static_cast<uint32_t>(strikes.size() % 2)});
        }
    }

    std::vector<rhythm::GameplayAction> actions;
    for (size_t s = 0; s < strikes.size(); ++s)
    {
        const CircleNote& note = chart.Notes[strikes[s].Note];
        const uint32_t pointer = strikes[s].Pointer;
        const bool isSlider = note.Type == NoteType::Slider;
        const glm::vec2 head = isSlider ? GetPathPosition(chart, note, 0.0f) : note.GetStackedPosition();
        actions.push_back(MakeAction(CircleAction::Press, pointer, head, note.Time));

        SongTimeUs release = isSlider ? note.EndTime : note.Time + AUTOPLAY_RELEASE_US;
        if (isSlider)
        {
            for (SongTimeUs t = note.Time + AUTOPLAY_MOVE_STEP_US; t < note.EndTime; t += AUTOPLAY_MOVE_STEP_US)
            {
                actions.push_back(MakeAction(CircleAction::Move, pointer, GetBallPosition(chart, note, t), t));
            }
        }
        // Let go before this pointer's next press.
        if (s + 2 < strikes.size())
        {
            release = std::min(release, chart.Notes[strikes[s + 2].Note].Time - 1);
        }
        const glm::vec2 end = isSlider ? GetBallPosition(chart, note, release) : head;
        actions.push_back(MakeAction(CircleAction::Release, pointer, end, std::max(release, note.Time)));
    }
    std::stable_sort(actions.begin(), actions.end(),
                     [](const rhythm::GameplayAction& a, const rhythm::GameplayAction& b) { return a.Time < b.Time; });
    return actions;
}

} // namespace hyoshi::circle
