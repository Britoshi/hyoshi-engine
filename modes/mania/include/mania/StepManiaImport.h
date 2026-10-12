#pragma once

#include "mania/ManiaChart.h"

#include "core/Result.h"

#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::mania
{

// What ImportStepMania reads from one file.
struct StepManiaImportResult
{
    // Every chart in the file with a lane count Mania plays (1 to MAX_LANES), in file order.
    std::vector<ManiaChart> Charts;
    // The song's background image, relative to the file, or empty.
    std::string BackgroundFile;
    std::vector<std::string> Warnings;
};

// StepMania's chart files, .sm and .ssc (DESIGN.md section 13.6), as Mania charts: a lane for each
// of a chart's columns (4 for dance-single, 8 for dance-double, 5 for pump-single, and so on).
//
// - Tags are #KEY:value; and may run over several lines; // starts a comment. In .ssc, each
//   chart's tags follow its #NOTEDATA, and its own timing (BPMS, OFFSET, STOPS, ...), music, and
//   credit win over the song's. In .sm, each #NOTES has the chart's type, description,
//   difficulty, meter, radar values, and notes, separated by colons.
// - Notes are measures of 4 beats, separated by commas, each an even split of rows, a character
//   per column: 1 a tap, 2 and 4 a hold's and a roll's head (both holds here), 3 their tail, and
//   L a lift (a tap here). Mines (M), fakes (F), and the rest aren't notes.
// - Times follow #BPMS (beat=BPM), #STOPS (beat=seconds, after that beat's notes), #DELAYS
//   (beat=seconds, before them), and #WARPS (beat=beats skipped, whose notes are left out), from
//   beat 0 at -#OFFSET seconds. A non-positive BPM is skipped over like a warp (old charts warp
//   with negative BPMs). Notes scroll at one speed: #SCROLLS and #SPEEDS are left out.
// - The title is #TITLE (with #SUBTITLE), the artist #ARTIST, the charter the chart's #CREDIT (or
//   the song's), the difficulty name the difficulty and meter ("Challenge 12", with the chart type
//   first for other than dance-single), the preview #SAMPLESTART.
Result<StepManiaImportResult> ImportStepMania(std::string_view text);

// What ImportStepMania reads of each chart, before building it, for a song list: the same charts,
// in the same order, with their details, timing, and notes' times (rhythm::Chart::Notes), and their
// lane counts.
struct StepManiaChartInfo
{
    rhythm::Chart Info;
    uint32_t LaneCount = 0;
};

struct StepManiaInfo
{
    std::vector<StepManiaChartInfo> Charts;
    std::string BackgroundFile;
    // #GENRE, or empty: words to search by.
    std::string Genre;
};

Result<StepManiaInfo> ReadStepManiaInfo(std::string_view text);

} // namespace hyoshi::mania
