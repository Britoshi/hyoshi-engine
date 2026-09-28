#pragma once

#include "rhythm/Chart.h"
#include "rhythm/Judgment.h"

#include "core/Time.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyoshi::circle
{

// Charts are authored in a 512 by 384 playfield with y pointing down (osu!'s), whatever the
// screen. Judging happens in these units too, so a replay plays the same at any resolution.
constexpr float PLAYFIELD_WIDTH = 512.0f;
constexpr float PLAYFIELD_HEIGHT = 384.0f;

// Notes this close in time are a chord: struck together, so neither blocks nor misses the other,
// and they're drawn as one step of the pattern. .osu times are whole milliseconds, but authored
// charts needn't be.
constexpr SongTimeUs CHORD_TOLERANCE_US = 1'000;

enum class NoteType : uint8_t
{
    // Struck once.
    Circle,
    // Struck at its head, then followed along a path.
    Slider,
    // Spun for its length. Not judged yet: it neither scores nor breaks combo.
    Spinner
};

// How a slider's control points become a curve (osu!'s letters: B, L, C, P).
enum class CurveType : uint8_t
{
    // Bezier segments, split where a control point repeats.
    Bezier,
    Linear,
    Catmull,
    // A circular arc through exactly three points; anything else falls back to Bezier.
    PerfectCircle
};

struct CircleNote
{
    SongTimeUs Time = 0;
    // Sliders: when the last pass ends. Spinners: when spinning ends. Circles: Time.
    SongTimeUs EndTime = 0;
    // Where it's struck, in playfield units.
    glm::vec2 Position{0.0f};
    NoteType Type = NoteType::Circle;
    // Starts a new combo, whose numbers count from 1.
    bool IsNewCombo = false;

    // Sliders: the path as authored (the head first) and how far along it the slider goes, which
    // with the tempo and slider velocity sets its duration.
    CurveType Curve = CurveType::Bezier;
    uint32_t FirstControlPoint = 0;
    uint32_t ControlPointCount = 0;
    float Length = 0.0f;
    // Traversals of the path: 1 is head to tail, 2 back to the head, and so on.
    uint32_t Passes = 1;

    // Filled in by FinishCircleChart.
    // The number drawn on it, counting from 1 within its combo.
    uint32_t ComboNumber = 1;
    // Sliders: the path sampled at even spacing along its length, in CircleChart::Path.
    uint32_t FirstPathPoint = 0;
    uint32_t PathPointCount = 0;
    // osu!'s stacking: notes on top of each other in quick succession are pulled apart
    // diagonally. It moves the whole note, so everything that uses a position adds it.
    glm::vec2 StackOffset{0.0f};

    glm::vec2 GetStackedPosition() const
    {
        return Position + StackOffset;
    }
};

// Half-widths of the hit windows, from OverallDifficulty.
struct CircleWindows
{
    int64_t GreatUs = 0;
    int64_t GoodUs = 0;
    int64_t MehUs = 0;

    // Great, Good, or Bad (osu!'s 300, 100, 50) for a timing error inside MehUs.
    rhythm::Judgment Classify(int64_t errorUs) const;
};

// osu!'s difficulty settings and what they set.
struct CircleDifficulty
{
    float CircleSize = 5.0f;
    float ApproachRate = 5.0f;
    float OverallDifficulty = 5.0f;
    float HpDrainRate = 5.0f;
    // Hundreds of playfield units a slider covers per beat at 1x slider velocity.
    double SliderMultiplier = 1.4;
    double SliderTickRate = 1.0;
    // Notes closer in time than this fraction of the approach time can stack. 0 turns it off.
    float StackLeniency = 0.7f;

    // A circle's radius in playfield units.
    float GetRadius() const;
    // How long before its time a note appears (its approach circle shrinks over this).
    SongTimeUs GetApproachUs() const;
    CircleWindows GetWindows() const;
};

struct CircleChart
{
    // Metadata, audio, and timing. Its Notes stay empty: the notes are below.
    rhythm::Chart Info;
    CircleDifficulty Difficulty;
    // The background image, relative to the chart file, or empty.
    std::string BackgroundFile;
    // Silence the chart asks for before the audio starts.
    SongTimeUs AudioLeadInUs = 0;
    // Sorted by time.
    std::vector<CircleNote> Notes;
    std::vector<glm::vec2> ControlPoints;
    std::vector<glm::vec2> Path;

    // Judgments a full run produces: one per circle and slider. Spinners aren't judged.
    uint32_t GetJudgmentCount() const;
    // When the last note ends, which needn't be the last note to start.
    SongTimeUs GetEndTime() const;
};

// Fills in what follows from the notes: combo numbers, slider paths, and stack offsets. The notes
// must be sorted by time, and sliders must have their EndTime.
void FinishCircleChart(CircleChart& chart);

// A slider's path position at `progress` (0 is the head, 1 the tail) including its stack offset.
glm::vec2 GetPathPosition(const CircleChart& chart, const CircleNote& note, float progress);

// Where the slider ball is at `time`, clamped to the slider's span.
glm::vec2 GetBallPosition(const CircleChart& chart, const CircleNote& note, SongTimeUs time);

// The curve through `points`, sampled densely (not clipped to a length). For drawing a path that
// is still being authored, and for measuring one.
std::vector<glm::vec2> SampleCurve(const std::vector<glm::vec2>& points, CurveType curve);

} // namespace hyoshi::circle
