#pragma once

#include "circle/CircleChart.h"
#include "core/Result.h"
#include "core/Time.h"
#include "mania/ManiaChart.h"
#include "rhythm/Chart.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hyoshi::songs
{

// What to play: a chart file (.rchart.json, or an osu!mania or osu!standard .osu), or the built-in
// chart generated to fit the metronome track.
struct ChartSource
{
    std::string Path;
    // For the built-in chart: its length in beats.
    uint32_t DemoBeats = 0;

    bool IsDemo() const
    {
        return Path.empty();
    }
};

// Per-chart settings (offsets) are keyed by what the chart is, not its file, so they survive a
// re-import or a move.
std::string GetChartKey(const rhythm::ChartMetadata& metadata);

// The built-in chart follows the generated metronome track: 128 BPM, first beat at 0.5 s, two
// bars to get ready.
constexpr double DEMO_BPM = 128.0;
constexpr hyoshi::SongTimeUs DEMO_FIRST_BEAT_US = 500'000;
constexpr uint32_t DEFAULT_DEMO_BEATS = 256;
constexpr uint32_t DEMO_BEATS_PER_BAR = 4;

// A 4-key drill on the metronome's beats that cycles through quarter notes, eighth-note trills,
// holds, and chords, with a slower and a faster scroll section.
hyoshi::Result<hyoshi::mania::ManiaChart> MakeDemoChart(uint32_t beatCount);

struct LoadedChart
{
    hyoshi::mania::ManiaChart Chart;
    // The chart's audio and hit sound files resolve against this directory (with a trailing
    // separator, or empty).
    std::string Directory;
    std::vector<std::string> Warnings;
};

// Reads a .rchart.json, or converts an osu!mania .osu in memory.
hyoshi::Result<LoadedChart> LoadChartFile(const std::string& path);

struct LoadedCircleChart
{
    hyoshi::circle::CircleChart Chart;
    // The chart's audio and background resolve against this directory (with a trailing
    // separator, or empty).
    std::string Directory;
    std::vector<std::string> Warnings;
};

// Converts an osu!standard .osu in memory.
hyoshi::Result<LoadedCircleChart> LoadCircleChartFile(const std::string& path);

// The BPM that lasts longest in the chart, or 0 without timing.
double GetMainBpm(const hyoshi::mania::ManiaChart& chart);
double GetMainBpm(const hyoshi::circle::CircleChart& chart);

// When the last note ends.
hyoshi::SongTimeUs GetChartEnd(const hyoshi::mania::ManiaChart& chart);
hyoshi::SongTimeUs GetChartEnd(const hyoshi::circle::CircleChart& chart);

} // namespace hyoshi::songs
