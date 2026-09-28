#include "mania/ManiaJudge.h"

#include <algorithm>

namespace hyoshi::mania
{

namespace
{

// How long autoplay holds a tap.
constexpr SongTimeUs AUTOPLAY_TAP_US = 50'000;

} // namespace

rhythm::GameplayAction MakeAction(ManiaAction type, uint32_t lane, SongTimeUs time)
{
    rhythm::GameplayAction action;
    action.Time = time;
    action.Type = static_cast<uint16_t>(type);
    action.Index = static_cast<uint16_t>(lane);
    return action;
}

void ManiaJudge::Reset(const ManiaChart& maniaChart, const rhythm::HitWindows& hitWindows)
{
    chart = &maniaChart;
    windows = hitWindows;
    lanes.assign(chart->LaneCount, Lane{});
    states.assign(chart->Notes.size(), NoteState::Pending);
    time = INT64_MIN;
    unfinishedNotes = static_cast<uint32_t>(chart->Notes.size());
    firstUnfinished = 0;

    for (uint32_t i = 0; i < chart->Notes.size(); ++i)
    {
        lanes[chart->Notes[i].Lane].Notes.push_back(i);
    }
}

std::optional<SongTimeUs> ManiaJudge::GetDueTime(const Lane& lane) const
{
    if (lane.ActiveHold)
    {
        return chart->Notes[*lane.ActiveHold].EndTime;
    }
    if (lane.Next < lane.Notes.size())
    {
        // Pressing at exactly Time + BadUs is still a Bad; one microsecond later it's a miss.
        return chart->Notes[lane.Notes[lane.Next]].Time + windows.BadUs + 1;
    }
    return std::nullopt;
}

void ManiaJudge::Advance(SongTimeUs target, std::vector<rhythm::JudgmentEvent>& events)
{
    if (chart == nullptr || target <= time)
    {
        return;
    }

    // Process due judgments across lanes in time order (ties by lane), so combo is the same
    // however the advances are split.
    while (true)
    {
        Lane* dueLane = nullptr;
        SongTimeUs dueTime = 0;
        for (Lane& lane : lanes)
        {
            const std::optional<SongTimeUs> laneDue = GetDueTime(lane);
            if (laneDue && *laneDue <= target && (dueLane == nullptr || *laneDue < dueTime))
            {
                dueLane = &lane;
                dueTime = *laneDue;
            }
        }
        if (dueLane == nullptr)
        {
            break;
        }

        if (dueLane->ActiveHold)
        {
            const uint32_t note = *dueLane->ActiveHold;
            events.push_back({note, rhythm::Judgment::Perfect, 0, dueTime, true});
            dueLane->ActiveHold.reset();
            Finish(note, NoteState::Hit);
        }
        else
        {
            const uint32_t note = dueLane->Notes[dueLane->Next++];
            events.push_back({note, rhythm::Judgment::Miss, 0, dueTime, false});
            if (chart->Notes[note].IsHold())
            {
                events.push_back({note, rhythm::Judgment::Miss, 0, dueTime, true});
            }
            Finish(note, NoteState::Missed);
        }
    }

    time = target;
}

void ManiaJudge::ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events)
{
    if (chart == nullptr || action.Index >= lanes.size())
    {
        return;
    }

    const SongTimeUs actionTime = std::max(action.Time, time);
    Advance(actionTime, events);
    Lane& lane = lanes[action.Index];

    if (action.Type == static_cast<uint16_t>(ManiaAction::Press))
    {
        lane.IsHeld = true;
        if (lane.ActiveHold || lane.Next >= lane.Notes.size())
        {
            return;
        }

        const uint32_t note = lane.Notes[lane.Next];
        const int64_t error = actionTime - chart->Notes[note].Time;
        const std::optional<rhythm::Judgment> judgment = windows.Classify(error);
        if (!judgment)
        {
            // Too early for the next note. (Too late was already a miss in Advance.)
            return;
        }

        events.push_back({note, *judgment, error, actionTime, false});
        ++lane.Next;
        if (chart->Notes[note].IsHold())
        {
            lane.ActiveHold = note;
            states[note] = NoteState::Held;
        }
        else
        {
            Finish(note, NoteState::Hit);
        }
    }
    else if (action.Type == static_cast<uint16_t>(ManiaAction::Release))
    {
        lane.IsHeld = false;
        if (!lane.ActiveHold)
        {
            return;
        }

        // Advance already completed holds whose end has passed, so this release is early.
        const uint32_t note = *lane.ActiveHold;
        const int64_t error = actionTime - chart->Notes[note].EndTime;
        const std::optional<rhythm::Judgment> judgment = windows.Classify(error);
        events.push_back({note, judgment.value_or(rhythm::Judgment::Miss), judgment ? error : 0, actionTime, true});
        lane.ActiveHold.reset();
        Finish(note, judgment ? NoteState::Hit : NoteState::Missed);
    }
}

uint32_t ManiaJudge::GetFirstUnfinishedNote() const
{
    return firstUnfinished;
}

void ManiaJudge::Finish(uint32_t note, NoteState state)
{
    states[note] = state;
    --unfinishedNotes;
    while (firstUnfinished < states.size() &&
           (states[firstUnfinished] == NoteState::Hit || states[firstUnfinished] == NoteState::Missed))
    {
        ++firstUnfinished;
    }
}

std::vector<rhythm::GameplayAction> MakeAutoplayActions(const ManiaChart& chart)
{
    std::vector<rhythm::GameplayAction> actions;
    actions.reserve(chart.Notes.size() * 2);

    std::vector<std::optional<SongTimeUs>> nextInLane(chart.LaneCount);
    for (size_t i = chart.Notes.size(); i-- > 0;)
    {
        const ManiaNote& note = chart.Notes[i];
        SongTimeUs release = note.IsHold() ? note.EndTime : note.Time + AUTOPLAY_TAP_US;
        if (nextInLane[note.Lane])
        {
            release = std::min(release, *nextInLane[note.Lane] - 1);
        }
        nextInLane[note.Lane] = note.Time;

        actions.push_back(MakeAction(ManiaAction::Press, note.Lane, note.Time));
        actions.push_back(MakeAction(ManiaAction::Release, note.Lane, release));
    }

    std::sort(actions.begin(), actions.end(),
              [](const rhythm::GameplayAction& a, const rhythm::GameplayAction& b)
              {
                  if (a.Time != b.Time)
                  {
                      return a.Time < b.Time;
                  }
                  if (a.Type != b.Type)
                  {
                      return a.Type < b.Type;
                  }
                  return a.Index < b.Index;
              });
    return actions;
}

} // namespace hyoshi::mania
