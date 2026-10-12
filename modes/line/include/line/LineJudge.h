#pragma once

#include "line/LineChart.h"

#include "rhythm/Judgment.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::line
{

// The Line mode's gameplay actions (DESIGN.md section 12.5), in GameplayAction::Type. Index is the
// pointer: a finger, the mouse, or a key.
enum class LineAction : uint16_t
{
    // A pointer came down at a field position.
    Press,
    Move,
    Release,
    // A key went down: a touch that reaches every note wherever it is (for playing on a keyboard),
    // and a flick.
    KeyPress,
    KeyRelease,
    // A pointer came down that strikes nothing (its press went to another mode, in a chart that
    // mixes them): down all the same, it catches drags, flicks it swipes across, and holds.
    Touch
};

// Positions are stored in quarter field units, so replays keep sub-unit aim.
constexpr float ACTION_POSITION_SCALE = 4.0f;

rhythm::GameplayAction MakeAction(LineAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time);
glm::vec2 GetActionPosition(const rhythm::GameplayAction& action);

// Phigros's windows: Perfect and Good either side, and Bad only early, for taps and holds.
struct LineWindows
{
    int64_t PerfectUs = 80'000;
    int64_t GoodUs = 160'000;
    int64_t BadUs = 180'000;
};

// A touch reaches a note within this many X_UNITs of it along its line, however far off the line
// it is, as in Phigros.
constexpr float HIT_HALF_WIDTH = 1.6f;
// Pointers down are checked this often for drags and holds coming into reach.
constexpr SongTimeUs SWEEP_US = 10'000;

enum class NoteState : uint8_t
{
    Pending,
    // A hold struck and still held.
    Held,
    Hit,
    Missed
};

// Line judgment rules (DESIGN.md section 16.5), after Phigros. Deterministic (DESIGN.md section 14.5):
// the result depends only on the chart and the actions with their song times; where a note is for
// a touch is where its line puts it at the judge's time.
//
// - A press strikes the earliest tap or hold head within reach whose window holds it: Perfect
//   within 80 ms, Good within 160, Bad from 160 to 180 ms early. A hold is then held to its end
//   (a second event); letting go, or straying from it, before its last 100 ms misses the end,
//   unless another pointer down on it takes it over.
// - Any pointer down counts as holding, however long it has been down: a drag is caught by a
//   pointer down within reach at any moment in the 160 ms either side of its time, and a hold by
//   one within reach once it has arrived, up to 160 ms after (Perfect, as if struck on time).
//   Pointers that don't move are checked every SWEEP_US, as lines move under them. A flick is
//   caught by a swipe across it then (each move of a swipe catching the flick nearest in time).
//   Drags and flicks are Perfect, at their time or when caught after it. A press with such a note
//   due nearer in time than the tap or hold it would strike is for that note, and strikes nothing
//   (as Phira gives a touch to the note nearest in time).
// - A key reaches every note, as if pressed on it, and flicks.
// - What isn't struck or caught by 160 ms after its time is missed.
class LineJudge
{
public:
    void Reset(const LineChart& chart, const LineWindows& windows = {});

    // Judges everything due by `time`.
    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events);
    // Advances to the action's time (or stays, if it's older), then applies it.
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events);

    // When the note a press at `position` (field units) would strike at `time` is due, if any: so a
    // game mixing modes can give the press to the mode with the note due first.
    std::optional<SongTimeUs> GetPressTarget(glm::vec2 position, SongTimeUs time) const;
    // When the tap or hold a press at `position` would strike at `time` is due, if any: a drag or
    // flick needs no press (any pointer down catches it), so it needn't take one from another mode.
    std::optional<SongTimeUs> GetStrikeTarget(glm::vec2 position, SongTimeUs time) const;

    SongTimeUs GetTime() const
    {
        return time;
    }
    bool IsFinished() const
    {
        return unfinishedNotes == 0;
    }
    NoteState GetNoteState(uint32_t note) const
    {
        return states[note];
    }
    // Every note before this one is finished and past.
    uint32_t GetFirstActiveNote() const
    {
        return firstActive;
    }
    const LineWindows& GetWindows() const
    {
        return windows;
    }

private:
    struct Pointer
    {
        glm::vec2 Position{0.0f};
        bool IsDown = false;
        // A key: reaches every note.
        bool IsAnywhere = false;
        SongTimeUs LastMoveTime = INT64_MIN;
        std::optional<uint32_t> HeldNote;
    };

    enum class CheckType : uint8_t
    {
        // Pointers down catch what's in reach (every SWEEP_US while something can be caught).
        Sweep,
        // A caught drag or flick reaches its time.
        Due,
        Miss,
        HoldEnd
    };
    struct Check
    {
        SongTimeUs Time = 0;
        uint32_t Note = 0;
        CheckType Type = CheckType::Miss;
    };
    std::optional<Check> FindNextCheck(SongTimeUs limit) const;
    void RunCheck(const Check& check, std::vector<rhythm::JudgmentEvent>& events);

    bool IsInReach(const LineNote& note, const Pointer& pointer) const;
    // Whether a pointer down could catch the note: a drag in its window, a hold since it arrived.
    bool IsCatchable(uint32_t note) const;
    std::optional<uint32_t> FindStrike(const Pointer& pointer, SongTimeUs at) const;
    // The drag or flick in reach due nearest `at` that no pointer has caught yet.
    std::optional<uint32_t> FindNearestCatch(const Pointer& pointer, SongTimeUs at) const;
    // canStrike is false for Touch.
    void Press(uint32_t pointer, glm::vec2 position, bool isAnywhere, bool canStrike,
               std::vector<rhythm::JudgmentEvent>& events);
    void Release(uint32_t pointer, std::vector<rhythm::JudgmentEvent>& events);
    // Drags and arrived holds under a down pointer, and flicks under a swipe (or a key), are caught.
    void Catch(Pointer& pointer, bool isFlick, std::vector<rhythm::JudgmentEvent>& events);
    void CatchAll(std::vector<rhythm::JudgmentEvent>& events);
    // isStrike: the player's timing (a press on a tap or hold), not a note caught by presence.
    void Emit(uint32_t note, rhythm::Judgment result, int64_t errorUs, bool isTail, bool isStrike, SongTimeUs eventTime,
              std::vector<rhythm::JudgmentEvent>& events);
    void Finish(uint32_t note, NoteState state);
    void AdvanceFirstActive();
    Pointer& GetPointer(uint32_t pointer);

    const LineChart* chart = nullptr;
    LineWindows windows;
    std::vector<NoteState> states;
    std::vector<bool> finished;
    // Drags and flicks: caught, and when.
    std::vector<std::optional<SongTimeUs>> caught;
    // Holds: the head's result, which the end takes too.
    std::vector<rhythm::Judgment> holdResults;
    std::vector<Pointer> pointers;
    // When pointers down are next checked for what's come into reach.
    SongTimeUs nextSweep = INT64_MIN;
    SongTimeUs time = INT64_MIN;
    uint32_t unfinishedNotes = 0;
    uint32_t firstActive = 0;
};

// Perfectly timed and aimed actions: a pointer on each note at its time, held through holds,
// swiped across flicks. Each note takes the lowest pointer not busy with another. For demos and
// tests.
std::vector<rhythm::GameplayAction> MakeAutoplayActions(const LineChart& chart);

} // namespace hyoshi::line
