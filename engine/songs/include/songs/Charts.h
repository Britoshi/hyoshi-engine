#pragma once

#include "circle/CircleChart.h"
#include "core/Result.h"
#include "core/Time.h"
#include "line/LineChart.h"
#include "mania/ManiaChart.h"
#include "rhythm/Chart.h"

#include <optional>

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

// A chart of any mix of modes (DESIGN.md section 13.5), each played by its own game's rules:
// "mania" lanes (osu!mania's, ManiaJudge); "arcaea" lanes on a track in perspective (Arcaea's,
// ArcaeaJudge); and "phigros" notes on judgment lines (Phigros's, the Line mode). Each mode present
// has its own runtime chart of its notes.
struct LoadedMixedChart
{
    // Metadata, audio, timing, and Modes (the chart's modes, as its file lists them); the notes
    // are in the modes' charts.
    hyoshi::rhythm::Chart Info;
    std::optional<hyoshi::mania::ManiaChart> Mania;
    std::optional<hyoshi::mania::ManiaChart> Arcaea;
    std::optional<hyoshi::line::LineChart> Lines;
    // osu!standard circles, in a medley.
    std::optional<hyoshi::circle::CircleChart> Circles;
    // A medley's parts, in order: which modes play from Start until End. Empty for a chart whose
    // modes play together throughout.
    struct Part
    {
        std::vector<std::string> Modes;
        hyoshi::SongTimeUs Start = 0;
        hyoshi::SongTimeUs End = 0;
    };
    std::vector<Part> Parts;
    // The chart's audio and images resolve against this directory (with a trailing separator, or
    // empty).
    std::string Directory;
    // A background image, relative to Directory, or empty.
    std::string BackgroundFile;
    std::vector<std::string> Warnings;

    uint32_t GetNoteCount() const;
    // Judgment events a full run produces, all modes together.
    uint32_t GetJudgmentCount() const;
    // When the last note of any mode ends.
    hyoshi::SongTimeUs GetEnd() const;
};

// Reads a .rchart.json of any of those modes (format 1 or 2), an osu!mania .osu (mania), a chart
// of a StepMania file (mania; IsStepManiaFile), a Phigros chart package's info.yml or info.txt,
// whose chart it imports (line::ImportPhigros), or a medley (LoadMedleyFile).
hyoshi::Result<LoadedMixedChart> LoadMixedChartFile(const std::string& path);

// Marks notes due together with a note of another of the chart's modes (rhythm::SIMULTANEOUS_US),
// so they shine as chords do: a circle and a Phigros note at one time, say.
void MarkSimultaneousAcrossModes(LoadedMixedChart& chart);

// A Phigros package's info file, which stands for its chart in a song folder.
bool IsPhigrosInfoFile(const std::string& path);

// A medley (.medley.json): one song played in parts, each from another chart of it, as its own
// mode (lanes, Phigros lines, or osu!standard circles), all on one audio file. It names its audio
// ("audio", "audioOffsetMs" as a chart's offset), its "sources" (each a "name", a "chart" file
// beside it that LoadMixedChartFile reads or an osu!standard .osu, "audioLagMs", how far into the
// medley's audio the source's own audio starts, and optionally "offsetMs", an offset to sync the
// source chart by in place of its own, and "lanesAs", "mania" or "arcaea", to play its lanes as
// that mode's), and its "parts" (each a "source", a source's name or a list of them that play
// together, and "untilMs", the part playing from the end of the one before it until then, the last
// to the end). Each source keeps only its notes in its parts, moved onto the medley's clock; the
// chart's Parts say which modes play when. One source each can have lanes and lines; several can
// have circles (each its own parts), merged under the first's difficulty and colors. Also "title",
// "artist", "charter", "difficultyName", "pack", "background", and "previewMs".
bool IsMedleyFile(const std::string& path);
hyoshi::Result<LoadedMixedChart> LoadMedleyFile(const std::string& path);

// StepMania files (.sm, .ssc) hold several charts. A chart path names one of them by its place
// among the charts the file plays, from 0, after a '#' ("Song.ssc#2"); the file alone is its first.
bool IsStepManiaFile(const std::string& path);
size_t CountStepManiaCharts(const std::string& path);
std::string MakeStepManiaChartPath(const std::string& file, size_t index);

// The BPM that lasts longest in the chart, or 0 without timing.
double GetMainBpm(const hyoshi::rhythm::Chart& info, SongTimeUs chartEnd);
double GetMainBpm(const hyoshi::mania::ManiaChart& chart);
double GetMainBpm(const hyoshi::circle::CircleChart& chart);
double GetMainBpm(const LoadedMixedChart& chart);

// When the last note ends.
hyoshi::SongTimeUs GetChartEnd(const hyoshi::mania::ManiaChart& chart);
hyoshi::SongTimeUs GetChartEnd(const hyoshi::circle::CircleChart& chart);

} // namespace hyoshi::songs
