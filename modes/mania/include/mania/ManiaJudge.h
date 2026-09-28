#pragma once

#include "mania/ManiaChart.h"

#include "rhythm/Judgment.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::mania
{

// Mania's gameplay actions (DESIGN.md section 12.5), stored in GameplayAction::Type; the lane is
// GameplayAction::Index.
enum class ManiaAction : uint16_t
{
    Press,
    Release
};

rhythm::GameplayAction MakeAction(ManiaAction type, uint32_t lane, SongTimeUs time);

enum class NoteState : uint8_t
{
    Pending,
    // A hold whose press was judged and whose release is still to come.
    Held,
    Hit,
    Missed
};

// Mania judgment rules. Deterministic (DESIGN.md section 14.5): the result depends only on the
// chart, the windows, and the actions with their song times, never on frame timing.
//
// - A press judges the earliest pending note in its lane if it is within the Bad window, and is
//   ignored otherwise.
// - A note not pressed by the end of its Bad window is missed; a missed hold misses its release
//   too.
// - A hold still held at its end time completes with a Perfect release. Releasing early judges
//   the release by how early it was, and a release earlier than the Bad window is a miss.
//
// Live play calls Advance(now - guard) every frame, so notes are missed on time without waiting
// for input, and ProcessAction for each input. Replays call only ProcessAction and Advance(end).
// Both give the same events as long as no action is older than the judge's time; the caller
// clamps action times to GetTime() before recording them.
class ManiaJudge
{
public:
    void Reset(const ManiaChart& chart, const rhythm::HitWindows& windows = {});

    // Judges everything due by `time`, in time order: misses and completed holds. Time never
    // moves backwards; an earlier time does nothing.
    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events);

    // Advances to the action's time, or stays at the current time if the action is older.
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events);

    SongTimeUs GetTime() const
    {
        return time;
    }

    // Every note has been judged completely.
    bool IsFinished() const
    {
        return unfinishedNotes == 0;
    }

    NoteState GetNoteState(uint32_t note) const
    {
        return states[note];
    }

    bool IsLaneHeld(uint32_t lane) const
    {
        return lanes[lane].IsHeld;
    }

    // The earliest note that is not finished, so drawing can skip everything before it.
    uint32_t GetFirstUnfinishedNote() const;

private:
    struct Lane
    {
        // Indices of this lane's notes, in time order.
        std::vector<uint32_t> Notes;
        size_t Next = 0;
        bool IsHeld = false;
        std::optional<uint32_t> ActiveHold;
    };

    // The time the next automatic judgment in a lane falls due, if any.
    std::optional<SongTimeUs> GetDueTime(const Lane& lane) const;
    void Finish(uint32_t note, NoteState state);

    const ManiaChart* chart = nullptr;
    rhythm::HitWindows windows;
    std::vector<Lane> lanes;
    std::vector<NoteState> states;
    SongTimeUs time = INT64_MIN;
    uint32_t unfinishedNotes = 0;
    uint32_t firstUnfinished = 0;
};

// Perfectly timed actions for every note: a press on time, and a release at the end of a hold or
// shortly after a tap. For demos and tests.
std::vector<rhythm::GameplayAction> MakeAutoplayActions(const ManiaChart& chart);

} // namespace hyoshi::mania
