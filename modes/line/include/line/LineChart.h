#pragma once

#include "rhythm/Chart.h"
#include "rhythm/ChartFile.h"

#include "core/Result.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::line
{

// The Line mode (DESIGN.md section 16.5): notes on judgment lines that move, turn, and fade, as in
// Phigros, in Phigros's own units. The field is FIELD_WIDTH by FIELD_HEIGHT (or reshaped to the
// screen: LineChart::FieldSize), fit to the canvas, with y down; a chart places a line at a
// fraction of it with y from the bottom and turns it
// counterclockwise in degrees, as Phigros does. A note's x is along its line in X_UNITs (the
// field is 17.8 across), and it comes at the line from its line's floor position (speed integrated
// over seconds) ahead, in DISTANCE_UNITs.
constexpr float FIELD_WIDTH = 1920.0f;
constexpr float FIELD_HEIGHT = 1080.0f;
constexpr float X_UNIT = 0.05625f * FIELD_WIDTH;
constexpr float DISTANCE_UNIT = 0.6f * FIELD_HEIGHT;

// A line's size, in field units, at length and thickness 1: it runs far past the field.
constexpr float LINE_LENGTH = FIELD_WIDTH * 4.0f;
constexpr float LINE_THICKNESS = 6.0f;

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
    // How it's drawn: its length and thickness as multiples of LINE_LENGTH and LINE_THICKNESS (for a
    // picture, field units per pixel of the picture, along and across the line; negative mirrors
    // it), and its color, each channel 0 to 1 (without keys for any, the usual line color, or for a
    // picture, white: the color tints it).
    std::vector<LineKey> Length;
    std::vector<LineKey> Thickness;
    std::vector<LineKey> Red;
    std::vector<LineKey> Green;
    std::vector<LineKey> Blue;

    // An image drawn for the line instead of a bar (a Re:PhiEdit line's texture: a storyboard),
    // named relative to the chart's folder; empty for a bar. Its anchor, the point of it (fractions
    // from its left and its bottom) that sits on the line's position, turning with the line.
    std::string Picture;
    glm::vec2 Anchor{0.5f};

    // Filled in by BuildLineChart: the floor position at each speed key.
    std::vector<double> SpeedKeyFloors;
};

// A picture lines draw, and the most field units per pixel any of them draws it at (the largest
// it's shown, so it can be loaded no larger).
struct PictureUse
{
    std::string Name;
    glm::vec2 MaxUnitsPerPixel{0.0f};
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

    // Filled in by BuildLineChart: the line's floor position at Time and EndTime, and whether it's
    // due together with another note, on any line (rhythm::MarkSimultaneous), so it shines, as
    // Phigros highlights such notes.
    double FloorPosition = 0.0;
    double EndFloorPosition = 0.0;
    bool IsSimultaneous = false;

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
    float Length = 1.0f;
    float Thickness = 1.0f;
    // Its own color, if it has one; 0 to 1.
    std::optional<glm::vec3> Color;
};

struct LineChart
{
    // Everything but the notes, which are below.
    rhythm::Chart Info;
    std::vector<JudgeLine> Lines;
    // Sorted by time.
    std::vector<LineNote> Notes;
    // The field, in field units: FIELD_WIDTH by FIELD_HEIGHT, or reshaped to a screen
    // (GetScreenFieldSize), so the lines, at fractions of it, spread over all of a screen of any
    // shape, as Phigros's do, while notes and lines keep their size. Pictures stretch with it.
    glm::vec2 FieldSize{FIELD_WIDTH, FIELD_HEIGHT};

    // Judgment events a full run produces: one per note, and a second for a hold's end.
    uint32_t GetJudgmentCount() const;
};

float GetKeyValue(const std::vector<LineKey>& keys, SongTimeUs time, float fallback);
// On a field of `fieldSize` (the chart's FieldSize).
LineState GetLineState(const JudgeLine& line, SongTimeUs time, glm::vec2 fieldSize = {FIELD_WIDTH, FIELD_HEIGHT});
// The line's floor position at `time`: its speed integrated over seconds.
double GetFloorPosition(const JudgeLine& line, SongTimeUs time);

// The chart's pictures, each once.
std::vector<PictureUse> GetPictureUses(const LineChart& chart);

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
// "speed". A line is {"x", "y", "rotation", "opacity", "speed", "length", "thickness", "red",
// "green", "blue"}, each a number or a list of {"timeUs", "value"} keys. ReadNote fills `notes`' Line fields in order
// (the caller sets their times), and WriteSettings and WriteNote write `lines` and `notes`.
rhythm::ChartModeFormat MakeLineFormat(std::vector<JudgeLine>& lines, std::vector<LineNote>& notes);

// A .rchart.json with only Line notes.
Result<LineChart> ParseLineChart(std::string_view json);

} // namespace hyoshi::line
