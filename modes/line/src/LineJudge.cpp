#include "line/LineJudge.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace hyoshi::line
{

namespace
{

// Letting go of a hold this close to its end still holds it to the end.
constexpr SongTimeUs HOLD_END_LENIENCE_US = 100'000;
// A pointer holding a hold may stray this much farther than reach before it lets go.
constexpr float HOLD_STRAY_SCALE = 1.5f;
// A pointer moving at least this fast (field units per millisecond) is swiping: a flick.
constexpr float FLICK_SPEED = 1.0f;

// Autoplay lets go this long after a note, swipes this far over this long for a flick, and moves
// along a hold this often.
constexpr SongTimeUs AUTOPLAY_RELEASE_US = 30'000;
constexpr SongTimeUs AUTOPLAY_FLICK_US = 30'000;
constexpr SongTimeUs AUTOPLAY_DRAG_LEAD_US = 8'000;
constexpr float AUTOPLAY_FLICK_DISTANCE = 80.0f;
constexpr SongTimeUs AUTOPLAY_MOVE_STEP_US = 16'000;

bool IsStruck(NoteKind kind)
{
    return kind == NoteKind::Tap || kind == NoteKind::Hold;
}

// Where a note meets its line at `time`.
glm::vec2 GetHitPosition(const LineChart& chart, const LineNote& note, SongTimeUs time)
{
    const LineState state = GetLineState(chart.Lines[note.Line], time, chart.FieldSize);
    return state.Position + (state.Along * (note.X * X_UNIT));
}

} // namespace

rhythm::GameplayAction MakeAction(LineAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time)
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

void LineJudge::Reset(const LineChart& lineChart, const LineWindows& lineWindows)
{
    chart = &lineChart;
    windows = lineWindows;
    const size_t count = chart->Notes.size();
    states.assign(count, NoteState::Pending);
    finished.assign(count, false);
    caught.assign(count, std::nullopt);
    holdResults.assign(count, rhythm::Judgment::Miss);
    pointers.clear();
    nextSweep = INT64_MIN;
    time = INT64_MIN;
    unfinishedNotes = static_cast<uint32_t>(count);
    firstActive = 0;
}

void LineJudge::Advance(SongTimeUs target, std::vector<rhythm::JudgmentEvent>& events)
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

void LineJudge::ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events)
{
    Advance(action.Time, events);
    const glm::vec2 position = GetActionPosition(action);
    switch (static_cast<LineAction>(action.Type))
    {
    case LineAction::Press:
        Press(action.Index, position, false, true, events);
        break;
    case LineAction::KeyPress:
        Press(action.Index, position, true, true, events);
        break;
    case LineAction::Touch:
        Press(action.Index, position, false, false, events);
        break;
    case LineAction::Release:
    case LineAction::KeyRelease:
        GetPointer(action.Index).Position = position;
        Release(action.Index, events);
        break;
    case LineAction::Move:
    {
        Pointer& pointer = GetPointer(action.Index);
        // Fast enough since the last move, a swipe.
        const SongTimeUs elapsed = std::max<SongTimeUs>(time - pointer.LastMoveTime, 1);
        const float speed = glm::distance(position, pointer.Position) * 1000.0f / static_cast<float>(elapsed);
        const bool isFlick = pointer.IsDown && pointer.LastMoveTime != INT64_MIN && speed >= FLICK_SPEED;
        pointer.Position = position;
        pointer.LastMoveTime = time;
        // A hold lets go if its pointer strays off it.
        if (pointer.HeldNote && !pointer.IsAnywhere)
        {
            const LineNote& held = chart->Notes[*pointer.HeldNote];
            const LineState state = GetLineState(chart->Lines[held.Line], time, chart->FieldSize);
            if (std::abs(GetLineX(state, position) - held.X) > HIT_HALF_WIDTH * HOLD_STRAY_SCALE)
            {
                Release(action.Index, events);
                pointer.IsDown = true;
            }
        }
        if (pointer.IsDown)
        {
            Catch(pointer, isFlick, events);
        }
        break;
    }
    }
}

std::optional<SongTimeUs> LineJudge::GetPressTarget(glm::vec2 position, SongTimeUs at) const
{
    Pointer pointer;
    pointer.Position = position;
    pointer.IsDown = true;
    const SongTimeUs when = std::max(at, time);
    // A drag or flick the press would be for counts as much as a note it would strike.
    if (const std::optional<uint32_t> note = FindStrike(pointer, when))
    {
        return chart->Notes[*note].Time;
    }
    const std::optional<uint32_t> catchable = FindNearestCatch(pointer, when);
    return catchable ? std::optional<SongTimeUs>(chart->Notes[*catchable].Time) : std::nullopt;
}

std::optional<SongTimeUs> LineJudge::GetStrikeTarget(glm::vec2 position, SongTimeUs at) const
{
    Pointer pointer;
    pointer.Position = position;
    pointer.IsDown = true;
    const std::optional<uint32_t> note = FindStrike(pointer, std::max(at, time));
    return note ? std::optional<SongTimeUs>(chart->Notes[*note].Time) : std::nullopt;
}

// The earliest check due by `limit`. Every check of a note comes at or after its time less the Good
// window, so the scan stops at the first note after that.
std::optional<LineJudge::Check> LineJudge::FindNextCheck(SongTimeUs limit) const
{
    const std::vector<LineNote>& notes = chart->Notes;
    std::optional<Check> next;
    const auto consider = [&](SongTimeUs at, uint32_t note, CheckType type)
    {
        if (at <= limit && (!next || at < next->Time))
        {
            next = Check{at, note, type};
        }
    };
    const bool isAnyDown = std::any_of(pointers.begin(), pointers.end(), [](const Pointer& p) { return p.IsDown; });
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time - windows.GoodUs <= limit; ++i)
    {
        const LineNote& note = notes[i];
        if (finished[i])
        {
            continue;
        }
        if (states[i] == NoteState::Held)
        {
            consider(note.EndTime, i, CheckType::HoldEnd);
            continue;
        }
        // Pointers down are checked from when the note can first be caught to when it can't.
        const bool canSweep = note.Kind == NoteKind::Drag || note.IsHold();
        if (isAnyDown && canSweep && !caught[i])
        {
            const SongTimeUs sweep = std::max(nextSweep, note.IsHold() ? note.Time : note.Time - windows.GoodUs);
            if (sweep <= note.Time + windows.GoodUs)
            {
                consider(sweep, i, CheckType::Sweep);
            }
        }
        if (!IsStruck(note.Kind) && caught[i])
        {
            consider(note.Time, i, CheckType::Due);
            continue;
        }
        consider(note.Time + windows.GoodUs + 1, i, CheckType::Miss);
    }
    return next;
}

void LineJudge::RunCheck(const Check& check, std::vector<rhythm::JudgmentEvent>& events)
{
    time = std::max(time, check.Time);
    const LineNote& note = chart->Notes[check.Note];
    switch (check.Type)
    {
    case CheckType::Sweep:
        nextSweep = time + SWEEP_US;
        CatchAll(events);
        break;
    case CheckType::Due:
        Emit(check.Note, rhythm::Judgment::Perfect, 0, false, false, check.Time, events);
        Finish(check.Note, NoteState::Hit);
        break;
    case CheckType::Miss:
        // Stamped with the moment it became a miss. A hold misses its end with its head.
        Emit(check.Note, rhythm::Judgment::Miss, 0, false, false, check.Time, events);
        if (note.IsHold())
        {
            Emit(check.Note, rhythm::Judgment::Miss, 0, true, false, check.Time, events);
        }
        Finish(check.Note, NoteState::Missed);
        break;
    case CheckType::HoldEnd:
        for (Pointer& pointer : pointers)
        {
            if (pointer.HeldNote == check.Note)
            {
                pointer.HeldNote.reset();
            }
        }
        Emit(check.Note, holdResults[check.Note], 0, true, false, check.Time, events);
        Finish(check.Note, NoteState::Hit);
        break;
    }
}

bool LineJudge::IsCatchable(uint32_t index) const
{
    const LineNote& note = chart->Notes[index];
    if (states[index] != NoteState::Pending || caught[index] || std::abs(time - note.Time) > windows.GoodUs)
    {
        return false;
    }
    return note.Kind == NoteKind::Drag || (note.IsHold() && time >= note.Time);
}

bool LineJudge::IsInReach(const LineNote& note, const Pointer& pointer) const
{
    if (pointer.IsAnywhere)
    {
        return true;
    }
    const LineState state = GetLineState(chart->Lines[note.Line], time, chart->FieldSize);
    return std::abs(GetLineX(state, pointer.Position) - note.X) <= HIT_HALF_WIDTH;
}

std::optional<uint32_t> LineJudge::FindStrike(const Pointer& pointer, SongTimeUs at) const
{
    const std::vector<LineNote>& notes = chart->Notes;
    std::optional<uint32_t> best;
    float bestDistance = 0.0f;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time - windows.BadUs <= at; ++i)
    {
        const LineNote& note = notes[i];
        const int64_t error = at - note.Time;
        if (states[i] != NoteState::Pending || !IsStruck(note.Kind) || error < -windows.BadUs ||
            error > windows.GoodUs || !IsInReach(note, pointer))
        {
            continue;
        }
        const LineState state = GetLineState(chart->Lines[note.Line], time, chart->FieldSize);
        const float distance = pointer.IsAnywhere ? 0.0f : std::abs(GetLineX(state, pointer.Position) - note.X);
        // The earliest; the nearest of notes at the same time.
        if (!best || note.Time < notes[*best].Time || (note.Time == notes[*best].Time && distance < bestDistance))
        {
            best = i;
            bestDistance = distance;
        }
    }
    // A press for a drag or flick due sooner, there, is for that note: it doesn't strike a tap or
    // hold due later (as Phira gives a touch to the note nearest in time).
    if (best)
    {
        if (const std::optional<uint32_t> catchable = FindNearestCatch(pointer, at);
            catchable && std::abs(at - notes[*catchable].Time) < std::abs(at - notes[*best].Time))
        {
            return std::nullopt;
        }
    }
    return best;
}

std::optional<uint32_t> LineJudge::FindNearestCatch(const Pointer& pointer, SongTimeUs at) const
{
    const std::vector<LineNote>& notes = chart->Notes;
    std::optional<uint32_t> nearest;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time - windows.GoodUs <= at; ++i)
    {
        const LineNote& note = notes[i];
        if (IsStruck(note.Kind) || states[i] != NoteState::Pending || std::abs(at - note.Time) > windows.GoodUs ||
            !IsInReach(note, pointer))
        {
            continue;
        }
        if (!nearest || std::abs(at - note.Time) < std::abs(at - notes[*nearest].Time))
        {
            nearest = i;
        }
    }
    return nearest;
}

void LineJudge::Press(uint32_t pointerIndex, glm::vec2 position, bool isAnywhere, bool canStrike,
                      std::vector<rhythm::JudgmentEvent>& events)
{
    Pointer& pointer = GetPointer(pointerIndex);
    pointer.Position = position;
    pointer.IsDown = true;
    pointer.IsAnywhere = isAnywhere;
    pointer.LastMoveTime = time;

    if (const std::optional<uint32_t> note = canStrike ? FindStrike(pointer, time) : std::nullopt)
    {
        const LineNote& struck = chart->Notes[*note];
        const int64_t error = time - struck.Time;
        const int64_t distance = std::abs(error);
        const rhythm::Judgment result = distance <= windows.PerfectUs ? rhythm::Judgment::Perfect
                                        : distance <= windows.GoodUs  ? rhythm::Judgment::Good
                                                                      : rhythm::Judgment::Bad;
        Emit(*note, result, error, false, true, time, events);
        if (struck.IsHold())
        {
            states[*note] = NoteState::Held;
            holdResults[*note] = result;
            pointer.HeldNote = *note;
        }
        else
        {
            Finish(*note, NoteState::Hit);
        }
    }
    // A key also flicks.
    Catch(pointer, isAnywhere, events);
}

void LineJudge::Release(uint32_t pointerIndex, std::vector<rhythm::JudgmentEvent>& events)
{
    Pointer& pointer = GetPointer(pointerIndex);
    pointer.IsDown = false;
    if (!pointer.HeldNote)
    {
        return;
    }
    const uint32_t note = *pointer.HeldNote;
    pointer.HeldNote.reset();
    // Another pointer down on the hold, holding nothing else, takes it over.
    for (Pointer& other : pointers)
    {
        if (other.IsDown && !other.HeldNote && IsInReach(chart->Notes[note], other))
        {
            other.HeldNote = note;
            return;
        }
    }
    const bool isHeldEnough = time >= chart->Notes[note].EndTime - HOLD_END_LENIENCE_US;
    Emit(note, isHeldEnough ? holdResults[note] : rhythm::Judgment::Miss, 0, true, false, time, events);
    Finish(note, isHeldEnough ? NoteState::Hit : NoteState::Missed);
}

void LineJudge::Catch(Pointer& pointer, bool isFlick, std::vector<rhythm::JudgmentEvent>& events)
{
    const std::vector<LineNote>& notes = chart->Notes;
    const auto take = [&](uint32_t i)
    {
        caught[i] = time;
        // Caught late: at once. Early: when it arrives (CheckType::Due).
        if (time >= notes[i].Time)
        {
            Emit(i, rhythm::Judgment::Perfect, 0, false, false, time, events);
            Finish(i, NoteState::Hit);
        }
    };
    // Every drag under the pointer, a hold that has reached it (if it holds nothing), and of the
    // flicks under a swipe, the one nearest in time.
    std::optional<uint32_t> flick;
    for (uint32_t i = firstActive; i < notes.size() && notes[i].Time - windows.GoodUs <= time; ++i)
    {
        const LineNote& note = notes[i];
        const bool isFlickCaught = note.Kind == NoteKind::Flick && isFlick && states[i] == NoteState::Pending &&
                                   !caught[i] && std::abs(time - note.Time) <= windows.GoodUs;
        const bool isHeldCaught = note.IsHold() && !pointer.HeldNote && IsCatchable(i);
        if ((!isFlickCaught && !isHeldCaught && !(note.Kind == NoteKind::Drag && IsCatchable(i))) ||
            !IsInReach(note, pointer))
        {
            continue;
        }
        if (note.IsHold())
        {
            // As if struck on time, and held from now.
            Emit(i, rhythm::Judgment::Perfect, 0, false, false, time, events);
            states[i] = NoteState::Held;
            holdResults[i] = rhythm::Judgment::Perfect;
            pointer.HeldNote = i;
        }
        else if (note.Kind == NoteKind::Drag)
        {
            take(i);
        }
        else if (!flick || std::abs(time - note.Time) < std::abs(time - notes[*flick].Time))
        {
            flick = i;
        }
    }
    if (flick)
    {
        take(*flick);
    }
}

void LineJudge::CatchAll(std::vector<rhythm::JudgmentEvent>& events)
{
    for (Pointer& pointer : pointers)
    {
        if (pointer.IsDown)
        {
            Catch(pointer, false, events);
        }
    }
}

void LineJudge::Emit(uint32_t note, rhythm::Judgment result, int64_t errorUs, bool isTail, bool isStrike,
                     SongTimeUs eventTime, std::vector<rhythm::JudgmentEvent>& events)
{
    rhythm::JudgmentEvent event;
    event.NoteIndex = note;
    event.Result = result;
    event.Time = eventTime;
    event.IsTail = isTail;
    // Only a strike's timing is the player's.
    event.IsTimed = isStrike && !isTail && result != rhythm::Judgment::Miss;
    event.ErrorUs = event.IsTimed ? errorUs : 0;
    events.push_back(event);
}

void LineJudge::Finish(uint32_t note, NoteState state)
{
    states[note] = state;
    if (!finished[note])
    {
        finished[note] = true;
        --unfinishedNotes;
    }
}

void LineJudge::AdvanceFirstActive()
{
    const std::vector<LineNote>& notes = chart->Notes;
    while (firstActive < notes.size() && finished[firstActive] &&
           time > std::max(notes[firstActive].EndTime, notes[firstActive].Time) + windows.GoodUs)
    {
        ++firstActive;
    }
}

LineJudge::Pointer& LineJudge::GetPointer(uint32_t pointer)
{
    if (pointer >= pointers.size())
    {
        pointers.resize(pointer + 1);
    }
    return pointers[pointer];
}

std::vector<rhythm::GameplayAction> MakeAutoplayActions(const LineChart& chart)
{
    std::vector<rhythm::GameplayAction> actions;
    std::vector<SongTimeUs> busyUntil;
    for (const LineNote& note : chart.Notes)
    {
        const bool isFlick = note.Kind == NoteKind::Flick;
        // Down on a drag just before it arrives, so the press is for it rather than a tap after it.
        const SongTimeUs start = isFlick                       ? note.Time - AUTOPLAY_FLICK_US
                                 : note.Kind == NoteKind::Drag ? note.Time - AUTOPLAY_DRAG_LEAD_US
                                                               : note.Time;
        uint32_t pointer = 0;
        while (pointer < busyUntil.size() && busyUntil[pointer] >= start)
        {
            ++pointer;
        }
        if (pointer == busyUntil.size())
        {
            busyUntil.push_back(INT64_MIN);
        }

        const glm::vec2 hit = GetHitPosition(chart, note, note.Time);
        if (isFlick)
        {
            // A swipe up across the note as it arrives.
            const LineState state = GetLineState(chart.Lines[note.Line], note.Time, chart.FieldSize);
            actions.push_back(MakeAction(LineAction::Press, pointer, GetHitPosition(chart, note, start), start));
            actions.push_back(
                MakeAction(LineAction::Move, pointer, hit + (state.Up * AUTOPLAY_FLICK_DISTANCE), note.Time));
            busyUntil[pointer] = note.Time + AUTOPLAY_RELEASE_US;
            actions.push_back(MakeAction(LineAction::Release, pointer, hit, busyUntil[pointer]));
            continue;
        }
        actions.push_back(MakeAction(LineAction::Press, pointer, GetHitPosition(chart, note, start), start));
        if (note.IsHold())
        {
            // On the hold where it meets its line, however the line moves.
            for (SongTimeUs t = note.Time + AUTOPLAY_MOVE_STEP_US; t < note.EndTime; t += AUTOPLAY_MOVE_STEP_US)
            {
                actions.push_back(MakeAction(LineAction::Move, pointer, GetHitPosition(chart, note, t), t));
            }
            busyUntil[pointer] = note.EndTime;
        }
        else
        {
            busyUntil[pointer] = note.Time + AUTOPLAY_RELEASE_US;
        }
        actions.push_back(MakeAction(LineAction::Release, pointer, GetHitPosition(chart, note, busyUntil[pointer]),
                                     busyUntil[pointer]));
    }
    std::stable_sort(actions.begin(), actions.end(),
                     [](const rhythm::GameplayAction& a, const rhythm::GameplayAction& b) { return a.Time < b.Time; });
    return actions;
}

} // namespace hyoshi::line
