#pragma once

#include "rhythm/Chart.h"
#include "rhythm/ChartFile.h"

#include "core/Result.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace hyoshi::line
{

// The Line mode (DESIGN.md section 16.5): notes on judgment lines that move, turn, and fade, as in
// Phigros, in Phigros's own units. The field is FIELD_WIDTH by FIELD_HEIGHT, fit to the canvas,
// with y down; a chart places a line at a fraction of it with y from the bottom and turns it
// counterclockwise in degrees, as Phigros does. A note's x is along its line in X_UNITs (the
// field is 17.8 across), and it comes at the line from its line's floor position (speed integrated
// over seconds) ahead, in DISTANCE_UNITs.
constexpr float FIELD_WIDTH = 1920.0f;
constexpr float FIELD_HEIGHT = 1080.0f;
constexpr float X_UNIT = 0.05625f * FIELD_WIDTH;
constexpr float DISTANCE_UNIT = 0.6f * FIELD_HEIGHT;

// A line's defaults: across the lower part of the field, level, seen, at speed 1.
constexpr float DEFAULT_LINE_X = 0.5f;
constexpr float DEFAULT_LINE_Y = 0.25f;
constexpr float DEFAULT_LINE_SPEED = 1.0f;

enum class NoteKind : uint8_t
{
    // Struck by a touch on it, graded by timing.
    Tap,
    // Struck like a tap, then held to its end.
    Hold,
    // Caught by any touch on it as it arrives, struck or not.
    Drag,
    // Caught by a swipe across it as it arrives.
    Flick
};

// A value over time: before the first key, its value; between keys, linear (or held, for speed);
// after the last, its value.
struct LineKey
{
    SongTimeUs Time = 0;
    float Value = 0.0f;
};

struct JudgeLine
{
    // Fractions of the field, y from the bottom.
    std::vector<LineKey> X;
    std::vector<LineKey> Y;
    // Degrees, counterclockwise.
    std::vector<LineKey> Rotation;
    // 0 to 1.
    std::vector<LineKey> Opacity;
    // How fast notes come at it, held from key to key.
    std::vector<LineKey> Speed;

    // Filled in by BuildLineChart: the floor position at each speed key.
    std::vector<double> SpeedKeyFloors;
};

struct LineNote
{
    SongTimeUs Time = 0;
    // Holds: when they end. Others: Time.
    SongTimeUs EndTime = 0;
    uint32_t Line = 0;
    // Along the line from its middle, in X_UNITs, positive toward the line's right.
    float X = 0.0f;
    // Comes from the line's upper side (its top when level), or else the lower.
    bool IsAbove = true;
    NoteKind Kind = NoteKind::Tap;
    // Scales how far ahead of the line the note is, so it comes faster or slower than the line's
    // speed.
    float Speed = 1.0f;

    // Filled in by BuildLineChart: the line's floor position at Time and EndTime.
    double FloorPosition = 0.0;
    double EndFloorPosition = 0.0;

    bool IsHold() const
    {
        return Kind == NoteKind::Hold && EndTime > Time;
    }
};

// Where a line is at a moment, in field units with y down: its middle, its direction along it
// (to its right), its upper side's direction, and how much it shows.
struct LineState
{
    glm::vec2 Position{0.0f};
    glm::vec2 Along{1.0f, 0.0f};
    glm::vec2 Up{0.0f, -1.0f};
    // Radians, clockwise on screen.
    float Angle = 0.0f;
    float Opacity = 1.0f;
};

struct LineChart
{
    // Everything but the notes, which are below.
    rhythm::Chart Info;
    std::vector<JudgeLine> Lines;
    // Sorted by time.
    std::vector<LineNote> Notes;

    // Judgment events a full run produces: one per note, and a second for a hold's end.
    uint32_t GetJudgmentCount() const;
};

float GetKeyValue(const std::vector<LineKey>& keys, SongTimeUs time, float fallback);
LineState GetLineState(const JudgeLine& line, SongTimeUs time);
// The line's floor position at `time`: its speed integrated over seconds.
double GetFloorPosition(const JudgeLine& line, SongTimeUs time);

// How far a note is from its line at `time`, in field units: positive before it gets there.
float GetNoteDistance(const LineChart& chart, const LineNote& note, SongTimeUs time);
// A note's head (or, with `isEnd`, a hold's end) at `time`, in field units.
glm::vec2 GetNotePosition(const LineChart& chart, const LineNote& note, SongTimeUs time, bool isEnd = false);
// A field point's position along a line, in X_UNITs from its middle, as a note's x is.
float GetLineX(const LineState& state, glm::vec2 point);

// Builds the runtime chart from the common chart and the notes' Line fields (one per chart note,
// in order). Rejects notes on lines that don't exist and holds without a length.
Result<LineChart> BuildLineChart(rhythm::Chart chart, std::vector<JudgeLine> lines, std::vector<LineNote> notes);

// The "phigros" mode's part of the chart format (Line notes): modeSettings {"lines": [...]} and a
// note's "line", "x", "side" ("above" or "below"), "kind" ("tap", "hold", "drag", "flick"), and
// "speed". A line is {"x", "y", "rotation", "opacity", "speed"}, each a number or a list of
// {"timeUs", "value"} keys. ReadNote fills `notes`' Line fields in order (the caller sets their
// times), and WriteSettings and WriteNote write `lines` and `notes`.
rhythm::ChartModeFormat MakeLineFormat(std::vector<JudgeLine>& lines, std::vector<LineNote>& notes);

// A .rchart.json with only Line notes.
Result<LineChart> ParseLineChart(std::string_view json);

} // namespace hyoshi::line
