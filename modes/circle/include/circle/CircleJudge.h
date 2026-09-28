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
    // The pointer moved.
    Move
};

// Positions are stored in 1/64 playfield units, so replays keep sub-unit aim.
constexpr float ACTION_POSITION_SCALE = 64.0f;

rhythm::GameplayAction MakeAction(CircleAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time);
glm::vec2 GetActionPosition(const rhythm::GameplayAction& action);

enum class NoteState : uint8_t
{
    Pending,
    // A slider whose head was struck; its pointer is following it.
    Held,
    // Struck, or a held slider that has ended or been let go. Hidden from then on.
    Hit,
    // Never struck. A missed slider's body stays until it ends.
    Missed
};

// Circle judgment rules, ported from osu!lazer's StartTimeOrderedHitPolicy. Deterministic (DESIGN.md
// section 14.5): the result depends only on the chart and the actions with their song times.
//
// - A press strikes the earliest pending note under the pointer whose Bad window contains it
//   (time order, never the nearest): a circle, or a slider head if the pointer isn't already
//   following a slider. Circles are graded by timing; a slider head is always a Great, and the
//   slider then only has to be let go (there are no ticks or tail judgments yet).
// - Note lock: a press that would strike a note while an earlier circle is still pending is
//   blocked, and does nothing else, if it comes before that circle's time. After it, the press
//   strikes its note and every earlier pending circle is missed. Notes at the same time (within
//   1 ms) are a chord: neither blocks or misses the other.
// - A note not struck by the end of its Bad window is missed. Spinners aren't judged.
//
// Live play calls Advance(now - guard) every frame and ProcessAction for each input. Replays call
// ProcessAction and Advance(end). Both give the same events as long as no action is older than the
// judge's time; the caller clamps action times to GetTime() before recording them.
class CircleJudge
{
public:
    void Reset(const CircleChart& chart);

    // Misses everything due by `time`, and lets go of sliders that have ended.
    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events);

    // Advances to the action's time (or stays, if it's older), then applies it.
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events);

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
    struct Pointer
    {
        glm::vec2 Position{0.0f};
        bool IsDown = false;
        std::optional<uint32_t> HeldSlider;
    };

    // What a press at the current time and `position` strikes, if anything.
    struct Target
    {
        uint32_t Note = 0;
        bool IsBlocked = false;
    };
    std::optional<Target> FindTarget(glm::vec2 position, bool canTakeSlider) const;
    void Press(uint32_t pointer, glm::vec2 position, std::vector<rhythm::JudgmentEvent>& events);
    void Release(uint32_t pointer);
    void MissCirclesBefore(uint32_t note, std::vector<rhythm::JudgmentEvent>& events);
    void Judge(uint32_t note, rhythm::Judgment result, int64_t errorUs, NoteState state, SongTimeUs eventTime,
               std::vector<rhythm::JudgmentEvent>& events);
    void Finish(uint32_t note);
    Pointer& GetPointer(uint32_t pointer);
    bool IsResolved(uint32_t note) const
    {
        return states[note] != NoteState::Pending;
    }

    const CircleChart* chart = nullptr;
    CircleWindows windows;
    float radius = 0.0f;
    std::vector<NoteState> states;
    // Finished: judged, and for a held slider, let go.
    std::vector<bool> finished;
    std::vector<Pointer> pointers;
    SongTimeUs time = INT64_MIN;
    uint32_t unfinishedNotes = 0;
    uint32_t firstActive = 0;
};

// Perfectly timed and aimed actions: the pointer moves onto each note and presses on time, follows
// sliders, and lets go shortly after. Two pointers alternate, so chords work. For demos and tests.
std::vector<rhythm::GameplayAction> MakeAutoplayActions(const CircleChart& chart);

} // namespace hyoshi::circle
