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

// A slider is followed while a pointer is down within this many radii of its ball, once following
// has started within one radius (osu!'s follow circle).
constexpr float FOLLOW_RADIUS_SCALE = 2.4f;

// osu!'s lenience at a slider's end: its tail is judged this long before the ball gets there, or
// halfway through the slider if that's later.
constexpr SongTimeUs TAIL_LENIENCE_US = 36'000;

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

// osu!'s sample banks: three sets of hit sounds.
enum class SampleBank : uint8_t
{
    Normal,
    Soft,
    Drum
};

// Sounds played over the normal one (osu!'s additions), as bits of HitSound::Additions.
constexpr uint8_t ADDITION_WHISTLE = 1U << 0U;
constexpr uint8_t ADDITION_FINISH = 1U << 1U;
constexpr uint8_t ADDITION_CLAP = 1U << 2U;

// The bank, custom sample index, and volume from a moment on, as osu!'s timing points set them.
struct SamplePoint
{
    SongTimeUs Time = 0;
    SampleBank Bank = SampleBank::Normal;
    // 0: the default sounds; from 1, the chart's own files first (osu!'s custom sample index).
    uint32_t CustomIndex = 0;
    // Percent, 0 to 100.
    uint32_t Volume = 100;
};

// A sound as the chart gives it: the normal sound and its additions. What it leaves unset comes from
// the sample point in effect (GetHitSound).
struct HitSound
{
    // Unset: the sample point's bank.
    std::optional<SampleBank> Bank;
    // Unset: the normal sound's bank.
    std::optional<SampleBank> AdditionBank;
    uint8_t Additions = 0;
    // 0: the sample point's.
    uint32_t CustomIndex = 0;
    // Percent; 0: the sample point's.
    uint32_t Volume = 0;
    // A sound file of the chart's, relative to it, that plays instead of all of the above.
    std::string File;
};

// A hit sound with the sample point applied: what plays.
struct ResolvedHitSound
{
    SampleBank Bank = SampleBank::Normal;
    SampleBank AdditionBank = SampleBank::Normal;
    uint8_t Additions = 0;
    uint32_t CustomIndex = 0;
    // 0 to 1.
    float Volume = 1.0f;
    std::string File;
};

// The parts of a slider that are judged, each on its own: its head is struck, and the rest are
// hit by following the slider when the ball reaches them.
enum class SliderPartType : uint8_t
{
    Head,
    // Along each pass, every tick distance (SliderTickRate per beat).
    Tick,
    // Where the ball turns back.
    Repeat,
    // The end of the last pass.
    Tail
};

struct SliderPart
{
    // When the ball reaches it, and its sound plays.
    SongTimeUs Time = 0;
    // When following is checked: Time, except for the tail (TAIL_LENIENCE_US).
    SongTimeUs JudgeTime = 0;
    SliderPartType Type = SliderPartType::Head;
    // Along the path: 0 is the head, 1 the end of the first pass.
    float Progress = 0.0f;
    // Its sound in CircleChart::HitSounds (a tick's is its slider's body sound), or NO_HIT_SOUND.
    uint32_t HitSound = 0;
};

constexpr uint32_t NO_HIT_SOUND = UINT32_MAX;

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
    // Sliders: the distance between ticks along the path, or 0 for none.
    float TickDistance = 0.0f;

    // Its sounds in CircleChart::HitSounds: a circle's one; a slider's body (for its ticks), then
    // one for each edge: the head, each turn, and the tail (Passes + 2 in all). Fewer than that
    // (none, for a chart made in code) leaves the rest with the sample point's normal sound.
    uint32_t FirstHitSound = 0;
    uint32_t HitSoundCount = 0;

    // Filled in by FinishCircleChart.
    // Sliders: their parts in time order, in CircleChart::SliderParts, the head first.
    uint32_t FirstPart = 0;
    uint32_t PartCount = 0;
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
    std::vector<HitSound> HitSounds;
    // Sorted by time. A note before the first takes the first; none at all is the normal bank at
    // full volume.
    std::vector<SamplePoint> SamplePoints;
    // Filled in by FinishCircleChart.
    std::vector<SliderPart> SliderParts;

    // Graded results a full run produces: one per circle and slider head. Spinners aren't judged.
    uint32_t GetJudgmentCount() const;
    // The combo of a run without a miss: one per circle and slider part.
    uint32_t GetMaxCombo() const;
    // When the last note ends, which needn't be the last note to start.
    SongTimeUs GetEndTime() const;
};

// Fills in what follows from the notes: combo numbers, slider paths and parts, and stack offsets.
// The notes must be sorted by time, and sliders must have their EndTime.
void FinishCircleChart(CircleChart& chart);

// A note's part (from 1, as JudgmentEvent::Part counts them), or nullptr for a note without parts.
const SliderPart* GetSliderPart(const CircleChart& chart, uint32_t note, uint32_t part);

// What sounds for a circle (part 0) or a slider part, with the sample point at its time applied
// (5 ms later, as osu! looks it up, so a line just after a note still counts).
ResolvedHitSound GetHitSound(const CircleChart& chart, uint32_t note, uint32_t part);

// A slider's path position at `progress` (0 is the head, 1 the tail) including its stack offset.
glm::vec2 GetPathPosition(const CircleChart& chart, const CircleNote& note, float progress);

// Where the slider ball is at `time`, clamped to the slider's span.
glm::vec2 GetBallPosition(const CircleChart& chart, const CircleNote& note, SongTimeUs time);

// The curve through `points`, sampled densely (not clipped to a length). For drawing a path that
// is still being authored, and for measuring one.
std::vector<glm::vec2> SampleCurve(const std::vector<glm::vec2>& points, CurveType curve);

} // namespace hyoshi::circle
