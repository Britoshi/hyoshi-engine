#include "mania/ArcaeaJudge.h"

#include <algorithm>
#include <cmath>

namespace hyoshi::mania
{

namespace
{

// A hold's combos come every half beat, or every beat from this tempo up.
constexpr double FAST_BPM = 255.0;
constexpr double DEFAULT_BPM = 120.0;

double GetBpmAt(const rhythm::Chart& info, SongTimeUs time)
{
    double bpm = info.Timing.empty() ? DEFAULT_BPM : info.Timing.front().Bpm;
    for (const rhythm::TimingPoint& point : info.Timing)
    {
        if (point.Time > time)
        {
            break;
        }
        bpm = point.Bpm;
    }
    return bpm > 0.0 ? bpm : DEFAULT_BPM;
}

// How many combos a hold gives, and how far apart they are.
struct HoldCombos
{
    uint32_t Count = 1;
    SongTimeUs Interval = 0;
};

HoldCombos GetHoldCombos(const ManiaChart& chart, const ManiaNote& note)
{
    const double bpm = GetBpmAt(chart.Info, note.Time);
    const double beatUs = 60'000'000.0 / bpm;
    const auto interval = static_cast<SongTimeUs>(std::llround(bpm >= FAST_BPM ? beatUs : beatUs * 0.5));
    const SongTimeUs length = note.EndTime - note.Time;
    HoldCombos combos;
    combos.Interval = std::max<SongTimeUs>(interval, 1);
    combos.Count = static_cast<uint32_t>(std::max<SongTimeUs>(length / combos.Interval, 1));
    return combos;
}

} // namespace

uint32_t ArcaeaJudge::GetJudgmentCount(const ManiaChart& chart)
{
    uint32_t count = 0;
    for (const ManiaNote& note : chart.Notes)
    {
        count += note.IsHold() ? GetHoldCombos(chart, note).Count : 1;
    }
    return count;
}

void ArcaeaJudge::Reset(const ManiaChart& maniaChart, const ArcaeaWindows& arcaeaWindows)
{
    chart = &maniaChart;
    windows = arcaeaWindows;
    lanes.assign(chart->LaneCount, Lane{});
    const size_t count = chart->Notes.size();
    states.assign(count, NoteState::Pending);
    finished.assign(count, false);
    holdHit.assign(count, false);
    ticks.clear();
    nextTick = 0;
    time = INT64_MIN;
    unfinishedNotes = static_cast<uint32_t>(count);
    firstUnfinished = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        const ManiaNote& note = chart->Notes[i];
        if (!note.IsHold())
        {
            continue;
        }
        const HoldCombos combos = GetHoldCombos(*chart, note);
        for (uint32_t k = 0; k < combos.Count; ++k)
        {
            Tick tick;
            tick.Time = note.Time + (static_cast<SongTimeUs>(k) * combos.Interval);
            // The first waits out the Far window, for a press just after the start.
            tick.CheckTime = k == 0 ? tick.Time + windows.FarUs : tick.Time;
            tick.Note = i;
            tick.IsFirst = k == 0;
            tick.IsLast = k + 1 == combos.Count;
            ticks.push_back(tick);
        }
    }
    std::stable_sort(ticks.begin(), ticks.end(),
                     [](const Tick& a, const Tick& b) { return a.CheckTime < b.CheckTime; });
}

void ArcaeaJudge::Advance(SongTimeUs target, std::vector<rhythm::JudgmentEvent>& events)
{
    if (chart == nullptr || target <= time)
    {
        return;
    }
    // Tap misses and hold combos, in time order.
    while (true)
    {
        std::optional<uint32_t> missedTap;
        SongTimeUs missTime = 0;
        for (uint32_t i = firstUnfinished; i < chart->Notes.size(); ++i)
        {
            const ManiaNote& note = chart->Notes[i];
            if (note.Time + windows.FarUs + 1 > target)
            {
                break;
            }
            if (!note.IsHold() && !finished[i])
            {
                missedTap = i;
                missTime = note.Time + windows.FarUs + 1;
                break;
            }
        }
        const bool hasTick = nextTick < ticks.size() && ticks[nextTick].CheckTime <= target;
        if (!missedTap && !hasTick)
        {
            break;
        }
        if (missedTap && (!hasTick || missTime <= ticks[nextTick].CheckTime))
        {
            time = std::max(time, missTime);
            events.push_back({*missedTap, rhythm::Judgment::Miss, 0, missTime, false});
            Finish(*missedTap, NoteState::Missed);
            continue;
        }
        const Tick tick = ticks[nextTick++];
        time = std::max(time, tick.CheckTime);
        const Lane& lane = lanes[chart->Notes[tick.Note].Lane];
        // The first combo also takes a press within Far of the start.
        const bool isHeld = lane.IsHeld || (tick.IsFirst && lane.LastPress != INT64_MIN &&
                                            std::abs(lane.LastPress - chart->Notes[tick.Note].Time) <= windows.FarUs);
        rhythm::JudgmentEvent event{tick.Note, isHeld ? rhythm::Judgment::Perfect : rhythm::Judgment::Miss, 0,
                                    tick.Time, false};
        event.IsTimed = false;
        events.push_back(event);
        holdHit[tick.Note] = holdHit[tick.Note] || isHeld;
        if (tick.IsLast)
        {
            Finish(tick.Note, holdHit[tick.Note] ? NoteState::Hit : NoteState::Missed);
        }
    }
    time = target;
}

void ArcaeaJudge::ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events)
{
    if (chart == nullptr || action.Index >= lanes.size())
    {
        return;
    }
    Advance(std::max(action.Time, time), events);
    Lane& lane = lanes[action.Index];
    if (action.Type == static_cast<uint16_t>(ManiaAction::Release))
    {
        lane.IsHeld = false;
        return;
    }
    lane.IsHeld = true;
    lane.LastPress = time;
    if (const std::optional<uint32_t> note = FindTap(action.Index, time))
    {
        const int64_t error = time - chart->Notes[*note].Time;
        const int64_t distance = std::abs(error);
        const rhythm::Judgment result = distance <= windows.MaxPureUs ? rhythm::Judgment::Perfect
                                        : distance <= windows.PureUs  ? rhythm::Judgment::Great
                                                                      : rhythm::Judgment::Good;
        events.push_back({*note, result, error, time, false});
        Finish(*note, NoteState::Hit);
    }
}

std::optional<SongTimeUs> ArcaeaJudge::GetPressTarget(uint32_t lane, SongTimeUs at) const
{
    if (chart == nullptr || lane >= lanes.size())
    {
        return std::nullopt;
    }
    if (const std::optional<uint32_t> tap = FindTap(lane, at))
    {
        return chart->Notes[*tap].Time;
    }
    // A hold under way that isn't held yet.
    for (uint32_t i = firstUnfinished; i < chart->Notes.size() && chart->Notes[i].Time - windows.FarUs <= at; ++i)
    {
        const ManiaNote& note = chart->Notes[i];
        if (note.Lane == lane && note.IsHold() && !finished[i] && at <= note.EndTime && !lanes[lane].IsHeld)
        {
            return note.Time;
        }
    }
    return std::nullopt;
}

NoteState ArcaeaJudge::GetNoteState(uint32_t note) const
{
    if (finished[note])
    {
        return states[note];
    }
    const ManiaNote& hold = chart->Notes[note];
    return hold.IsHold() && time >= hold.Time - windows.FarUs && lanes[hold.Lane].IsHeld ? NoteState::Held
                                                                                         : NoteState::Pending;
}

std::optional<uint32_t> ArcaeaJudge::FindTap(uint32_t lane, SongTimeUs at) const
{
    for (uint32_t i = firstUnfinished; i < chart->Notes.size() && chart->Notes[i].Time - windows.FarUs <= at; ++i)
    {
        const ManiaNote& note = chart->Notes[i];
        if (note.Lane == lane && !note.IsHold() && !finished[i] && std::abs(at - note.Time) <= windows.FarUs)
        {
            return i;
        }
    }
    return std::nullopt;
}

void ArcaeaJudge::Finish(uint32_t note, NoteState state)
{
    states[note] = state;
    if (!finished[note])
    {
        finished[note] = true;
        --unfinishedNotes;
    }
    while (firstUnfinished < finished.size() && finished[firstUnfinished])
    {
        ++firstUnfinished;
    }
}

} // namespace hyoshi::mania
