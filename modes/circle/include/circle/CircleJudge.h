#pragma once

#include "circle/CircleChart.h"

#include "rhythm/Judgment.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::circle
{

// The circle mode's gameplay actions (DESIGN.md section 12.5), in GameplayAction::Type. Index is
// the pointer: a finger, or the mouse with its buttons and keys as one pointer.
enum class CircleAction : uint16_t
{
    // A press edge where the pointer is: a button, a key, or a finger landing.
    Press,
    // The pointer let go (every button and key up, or the finger lifted).
    Release,
    // The pointer moved, down or not: a pointer crossing a flick fast enough strikes it either way.
    Move,
    // The pointer came down without a press: a finger down for another mode's note, in a game
    // mixing modes. It follows sliders, and strikes nothing.
    Touch
};

// Positions are stored in 1/64 playfield units, so replays keep sub-unit aim.
constexpr float ACTION_POSITION_SCALE = 64.0f;

rhythm::GameplayAction MakeAction(CircleAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time);
glm::vec2 GetActionPosition(const rhythm::GameplayAction& action);

// A circle's, or a slider's head's.
enum class NoteState : uint8_t
{
    Pending,
    // Struck. Hidden from then on; a slider's body and ball stay until it ends.
    Hit,
    // Never struck. A missed slider's body stays until it ends.
    Missed
};

// Circle judgment rules, ported from osu!lazer's: its StartTimeOrderedHitPolicy, and its sliders.
// Deterministic (DESIGN.md section 14.5): the result depends only on the chart and the
// actions with their song times.
//
// - A press strikes the earliest pending note under the pointer whose Bad window contains it
//   (time order, never the nearest): a circle, or a slider's head. Circles are graded by timing.
// - Note lock: a press that would strike a note while an earlier circle is still pending is
//   blocked, and does nothing else, if it comes before that circle's time. After it, the press
//   strikes its note and every earlier pending circle is missed. Notes at the same time (within
//   1 ms) are a chord: neither blocks or misses the other.
// - Heads that aren't pressed (HeadKind) strike as their pointers move, as a press does but always
//   a Great, untimed. A slide strikes when a pointer down is on it, from its time to the end of its
//   window: when a pointer presses (one that strikes or is blocked doesn't), moves, or touches
//   there, or at its time for a pointer already there. A flick strikes when a pointer, down or
//   not, moves across it within its window at its speed or faster: how far the pointer went over
//   the last FLICK_SPEED_WINDOW_US (staying where it last was between actions), once it's been
//   tracked for half that since it last let go. A flick is blocked as a press is. Neither blocks
//   later notes, nor is missed by one struck after it: it keeps its window.
// - A note not struck by the end of its Bad window is missed. Spinners aren't judged.
// - Sliders are judged in parts (CircleChart::SliderParts), each an event of its own
//   (JudgmentEvent::Part), with no result for the slider itself. The head is graded by timing as a
//   circle is (part 1, timed). Each tick, repeat, and the tail after it is hit or missed by whether
//   the slider is being followed when the ball gets there, once the head has been judged: a head
//   struck late judges the parts already passed by whether the press was within reach of them. A
//   slider is followed while a pointer is down within FOLLOW_RADIUS_SCALE radii of its ball, once
//   one has been down within a radius of it since the slider started; the head needn't be struck.
//
// Live play calls Advance(now - guard) every frame and ProcessAction for each input. Replays call
// ProcessAction and Advance(end). Both give the same events as long as no action is older than the
// judge's time; the caller clamps action times to GetTime() before recording them. Everything due
// by a time (misses, slider parts) is judged in time order, before any action at that time.
class CircleJudge
{
public:
    // Judges by `windows` if given, else by the chart's OverallDifficulty.
    void Reset(const CircleChart& chart, const std::optional<CircleWindows>& windows = std::nullopt);

    // Judges everything due by `time`: misses, and slider parts.
    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events);

    // Advances to the action's time (or stays, if it's older), then applies it.
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events);

    // When the note a press at `position` would strike at `time` is due, if any (a press note lock
    // blocks strikes nothing): so a game mixing modes can give the press to the mode with the note
    // due first.
    std::optional<SongTimeUs> GetPressTarget(glm::vec2 position, SongTimeUs time) const;

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
    // A slider is being followed, as of the judge's time or the last action.
    bool IsFollowing(uint32_t note) const
    {
        return following[note];
    }
    // A slider's next part to be judged by following, as JudgmentEvent::Part counts them (from 2:
    // the head is judged by a press instead); past PartCount once they all are.
    uint32_t GetNextPart(uint32_t note) const
    {
        return nextPart[note];
    }
    // Every note before this one is finished and past, so drawing and judging can start here.
    uint32_t GetFirstActiveNote() const
    {
        return firstActive;
    }
    const CircleWindows& GetWindows() const
    {
        return windows;
    }

private:
    struct Sample
    {
        SongTimeUs Time = 0;
        glm::vec2 Position{0.0f};
    };

    struct Pointer
    {
        glm::vec2 Position{0.0f};
        bool IsDown = false;
        // Where it's been since it last let go, over the last FLICK_SPEED_WINDOW_US (and the
        // place it was at the start of that).
        std::vector<Sample> Track;
    };

    // What a press at `position` at a time strikes, if anything.
    struct Target
    {
        uint32_t Note = 0;
        bool IsBlocked = false;
    };

    // Something due at a time: a note missed at the end of its window, a slider starting, a slider
    // part reached, or a slide's time, struck by a pointer already on it.
    enum class CheckType : uint8_t
    {
        Miss,
        SliderStart,
        SliderPart,
        SlideTime
    };
    struct Check
    {
        SongTimeUs Time = 0;
        uint32_t Note = 0;
        CheckType Type = CheckType::Miss;
    };
    std::optional<Check> FindNextCheck(SongTimeUs limit) const;
    void RunCheck(const Check& check, std::vector<rhythm::JudgmentEvent>& events);

    std::optional<Target> FindTarget(glm::vec2 position, SongTimeUs at) const;
    // Whether an earlier circle keeps a note from being struck now (note lock).
    bool IsBlocked(uint32_t note, SongTimeUs at) const;
    // Whether the press struck a note or was blocked.
    bool Press(glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events);
    // Slides under a pointer down at `position`, and flicks it crossed coming from `from`.
    void Slide(glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events);
    void Flick(const Pointer& pointer, glm::vec2 from, std::vector<rhythm::JudgmentEvent>& events);
    // Adds where the pointer is now to its track, keeping only what its speed needs.
    static void AddSample(Pointer& pointer, SongTimeUs at);
    // Playfield units per second, over the last FLICK_SPEED_WINDOW_US; 0 until it's been tracked
    // for half that.
    static float GetSpeed(const Pointer& pointer);
    // Strikes a head with a result: a circle's, or a slider's, judging the parts it's passed.
    void Strike(uint32_t note, glm::vec2 position, rhythm::Judgment result, int64_t errorUs, bool isTimed,
                std::vector<rhythm::JudgmentEvent>& events);
    void MissCirclesBefore(uint32_t note, std::vector<rhythm::JudgmentEvent>& events);
    // A circle's result.
    void Judge(uint32_t note, rhythm::Judgment result, int64_t errorUs, bool isTimed, NoteState state,
               SongTimeUs eventTime, std::vector<rhythm::JudgmentEvent>& events);
    void JudgePart(uint32_t note, uint32_t part, rhythm::Judgment result, int64_t errorUs, bool isTimed,
                   SongTimeUs eventTime, std::vector<rhythm::JudgmentEvent>& events);
    void JudgePassedParts(uint32_t note, glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events);
    void FinishSlider(uint32_t note);
    // Whether a slider is followed at `at`, given the pointers now; UpdateFollowing keeps it.
    bool IsFollowedAt(uint32_t note, SongTimeUs at) const;
    void UpdateFollowing(uint32_t note, SongTimeUs at);
    // After an action: every slider under way follows the pointers.
    void UpdateFollowingAll();
    void Finish(uint32_t note);
    void AdvanceFirstActive();
    Pointer& GetPointer(uint32_t pointer);
    bool IsResolved(uint32_t note) const
    {
        return states[note] != NoteState::Pending;
    }

    const CircleChart* chart = nullptr;
    CircleWindows windows;
    float radius = 0.0f;
    std::vector<NoteState> states;
    // Finished: everything judged, a slider's parts and result too.
    std::vector<bool> finished;
    // Sliders: started (followed from then on), followed now, and the next part to judge.
    std::vector<bool> started;
    std::vector<bool> following;
    std::vector<uint32_t> nextPart;
    // Slides: their time has been checked for pointers already on them.
    std::vector<bool> slideChecked;
    std::vector<Pointer> pointers;
    SongTimeUs time = INT64_MIN;
    uint32_t unfinishedNotes = 0;
    uint32_t firstActive = 0;
};

// Perfectly timed and aimed actions: a pointer moves onto each note and presses on time (sweeps
// across a flick, the way from the note before, and touches down if it's a slider's), follows
// sliders (on every part), and lets go shortly after, or when the slider ends. Each note takes
// the lowest pointer not busy with another, so chords and notes during a slider work. For demos
// and tests.
std::vector<rhythm::GameplayAction> MakeAutoplayActions(const CircleChart& chart);

} // namespace hyoshi::circle
