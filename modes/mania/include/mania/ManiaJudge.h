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
    Release,
    // The lane is held without a press (a finger whose press went to another mode, in a chart that
    // mixes them): it strikes nothing, but Arcaea's holds count it as holding. Mania's holds start
    // with a press, so ManiaJudge only marks the lane held.
    Touch
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

// What the game and the playfield need from a judge of lanes: Mania's (ManiaJudge) or Arcaea's
// (ArcaeaJudge).
class ILaneJudge
{
public:
    virtual ~ILaneJudge() = default;

    virtual void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events) = 0;
    virtual void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events) = 0;
    virtual SongTimeUs GetTime() const = 0;
    virtual bool IsFinished() const = 0;
    virtual NoteState GetNoteState(uint32_t note) const = 0;
    virtual bool IsLaneHeld(uint32_t lane) const = 0;
    // The earliest note that is not finished, so drawing can skip everything before it.
    virtual uint32_t GetFirstUnfinishedNote() const = 0;
    // When the note a press in `lane` at `time` would strike (or start holding) is due, if any:
    // for touch input that picks the note due first among several lanes or modes.
    virtual std::optional<SongTimeUs> GetPressTarget(uint32_t lane, SongTimeUs time) const = 0;
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
class ManiaJudge : public ILaneJudge
{
public:
    void Reset(const ManiaChart& chart, const rhythm::HitWindows& windows = {});

    // Judges everything due by `time`, in time order: misses and completed holds. Time never
    // moves backwards; an earlier time does nothing.
    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events) override;

    // Advances to the action's time, or stays at the current time if the action is older.
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events) override;

    std::optional<SongTimeUs> GetPressTarget(uint32_t lane, SongTimeUs time) const override;

    SongTimeUs GetTime() const override
    {
        return time;
    }

    // Every note has been judged completely.
    bool IsFinished() const override
    {
        return unfinishedNotes == 0;
    }

    NoteState GetNoteState(uint32_t note) const override
    {
        return states[note];
    }

    bool IsLaneHeld(uint32_t lane) const override
    {
        return lanes[lane].IsHeld;
    }

    // The earliest note that is not finished, so drawing can skip everything before it.
    uint32_t GetFirstUnfinishedNote() const override;

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

// osu!mania's windows for an overall difficulty (0 to 10): a MAX within 16 ms, and a 300, 200, and
// 100 within 64, 97, and 127 ms less 3 per OD, as Perfect, Great, Good, and Bad. Its 50s aren't
// judged: a press later than a 100 is ignored, as with any windows. OD 8 gives the defaults.
rhythm::HitWindows GetOsuManiaWindows(double overallDifficulty);

} // namespace hyoshi::mania
