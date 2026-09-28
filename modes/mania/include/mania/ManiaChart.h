#pragma once

#include "rhythm/Chart.h"
#include "rhythm/ScrollMap.h"

#include "core/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::mania
{

constexpr uint32_t MAX_LANES = 10;

// A note in the runtime chart (DESIGN.md section 14.1), with its scroll positions precomputed.
struct ManiaNote
{
    SongTimeUs Time = 0;
    // Equals Time for taps.
    SongTimeUs EndTime = 0;
    uint32_t Lane = 0;
    // Keysound: an index into Info.Samples, or rhythm::ChartNote::NO_SAMPLE.
    int32_t Sample = rhythm::ChartNote::NO_SAMPLE;
    uint32_t SampleVolume = 100;
    double ScrollPosition = 0.0;
    double EndScrollPosition = 0.0;

    bool IsHold() const
    {
        return EndTime > Time;
    }
};

struct ManiaChart
{
    // Everything but the notes, which are below.
    rhythm::Chart Info;
    uint32_t LaneCount = 4;
    // Sorted by time.
    std::vector<ManiaNote> Notes;
    rhythm::ScrollMap Scroll;

    // Judgment events a full run produces: one per tap, two per hold (press and release).
    uint32_t GetJudgmentCount() const;
};

// Builds the runtime chart from the common chart and one lane per note. Rejects lanes out of
// range and notes that start before the previous note in their lane has ended.
Result<ManiaChart> BuildManiaChart(rhythm::Chart chart, uint32_t laneCount, std::span<const uint32_t> lanes);

// .rchart.json with "mode": "mania", "modeSettings": {"laneCount": N}, and a "lane" per note.
Result<ManiaChart> ParseManiaChart(std::string_view json);
Result<std::string> WriteManiaChart(const ManiaChart& chart);

} // namespace hyoshi::mania
