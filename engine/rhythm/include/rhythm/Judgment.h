#pragma once

#include "core/Time.h"

#include <array>
#include <cstdint>
#include <optional>

namespace hyoshi::rhythm
{

// The shared judgment vocabulary (DESIGN.md section 14.3). Modes decide what counts as a hit.

enum class Judgment : uint8_t
{
    Perfect,
    Great,
    Good,
    Bad,
    Miss,
    Count
};

constexpr size_t JUDGMENT_COUNT = static_cast<size_t>(Judgment::Count);

const char* ToString(Judgment judgment);

// Half-widths in microseconds, inclusive. Starting points to tune, not final numbers.
struct HitWindows
{
    int64_t PerfectUs = 16'000;
    int64_t GreatUs = 40'000;
    int64_t GoodUs = 73'000;
    int64_t BadUs = 103'000;

    // The judgment for a timing error, or nothing when it is outside every window.
    std::optional<Judgment> Classify(int64_t errorUs) const;
};

struct JudgmentEvent
{
    uint32_t NoteIndex = 0;
    Judgment Result = Judgment::Miss;
    // Negative is early, positive is late. 0 for misses.
    int64_t ErrorUs = 0;
    // When the judgment happened in song time.
    SongTimeUs Time = 0;
    // The release at the end of a note with a duration, rather than its start.
    bool IsTail = false;
};

// A mode-specific player action with its song time (DESIGN.md section 12.5). Replays are lists
// of these, so fields are fixed-size integers; positions are quantized by the mode.
struct GameplayAction
{
    SongTimeUs Time = 0;
    uint16_t Type = 0;
    uint16_t Index = 0;
    int32_t X = 0;
    int32_t Y = 0;

    friend bool operator==(const GameplayAction&, const GameplayAction&) = default;
};

} // namespace hyoshi::rhythm
