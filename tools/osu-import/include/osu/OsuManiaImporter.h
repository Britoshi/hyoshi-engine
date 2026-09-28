#pragma once

#include "mania/ManiaChart.h"

#include "core/Result.h"

#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::osu
{

// Converts an osu!mania beatmap (.osu text) to a Mania chart, for local testing with real charts
// (DESIGN.md section 13.4). Imported charts stay local unless their author permits otherwise.
//
// - Lanes: floor(x * keys / 512), with the key count from CircleSize.
// - Holds: type bit 128, end time in the extras.
// - Timing: every uninherited point becomes a timing point.
// - Scroll velocity: inherited points set it (-100 / beatLength, clamped to 0.1..10, as osu!
//   does), uninherited points reset it to 1, and BPM changes scale it relative to the most
//   common BPM, the way osu!mania scrolls.
// - Files older than format v5 get osu!'s 24 ms offset.
// - A note starting before the previous note in its lane ends is dropped with a warning.
struct OsuImportResult
{
    mania::ManiaChart Chart;
    std::vector<std::string> Warnings;
};

Result<OsuImportResult> ImportOsuMania(std::string_view osuText);

} // namespace hyoshi::osu
