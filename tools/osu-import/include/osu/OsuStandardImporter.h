#pragma once

#include "circle/CircleChart.h"

#include "core/Result.h"

#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::osu
{

// Converts an osu!standard beatmap (.osu text, Mode 0) to a circle chart, for local testing with
// real charts (DESIGN.md section 13.4). Imported charts stay local unless their author permits
// otherwise.
//
// - Notes: circles, sliders (curve type, control points, passes, length), and spinners, with new
//   combos; the first note, and the note after a spinner, always start one (as osu! does).
// - Slider durations: length / (SliderMultiplier * 100 * slider velocity) beats at the tempo in
//   effect. Green lines set the slider velocity (-100 / beatLength, clamped to 0.1..10) and red
//   lines reset it to 1, as osu! does.
// - Difficulty: CircleSize, ApproachRate (OverallDifficulty when missing, as in old files),
//   OverallDifficulty, HPDrainRate, SliderMultiplier, SliderTickRate, and StackLeniency (from
//   [General], 0.7 when missing).
// - Timing: every red line becomes a timing point; slider velocity goes to ScrollVelocity.
// - The background image from [Events], AudioLeadIn, and PreviewTime.
// - Files older than format v5 get osu!'s 24 ms offset.
// - A slider with a single control point is imported as a circle, with a warning. Curve type
//   changes inside a slider (lazer's newer format) are read as one curve, with a warning.
struct OsuStandardImportResult
{
    circle::CircleChart Chart;
    std::vector<std::string> Warnings;
};

Result<OsuStandardImportResult> ImportOsuStandard(std::string_view osuText);

} // namespace hyoshi::osu
