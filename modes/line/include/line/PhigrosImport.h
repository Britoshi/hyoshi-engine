#pragma once

#include "line/LineChart.h"

#include "core/Result.h"

#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::line
{

struct PhigrosImportResult
{
    LineChart Chart;
    std::vector<std::string> Warnings;
};

// Converts a chart in Phigros's own format to a Line chart (DESIGN.md section 16.5), for local
// testing: formatVersion 3 (or 1, whose move events pack x and y into one number), with each of
// judgeLineList's lines bringing its bpm (times are in 1/32 beats of it), notesAbove and notesBelow
// (type 1 tap, 2 drag, 3 hold, 4 flick; positionX, holdTime, speed), and speed, move, rotate, and
// disappear events, which become the line's keys (rotation as Phigros turns it, counterclockwise).
// floorPosition is computed again from the speed events. "offset" (seconds) becomes the chart
// offset. Metadata and audio come from the package's info (ReadPhigrosInfo).
Result<PhigrosImportResult> ImportPhigros(std::string_view json);

// A Phigros chart package's info: Phira's info.yml (name, composer, charter, level, music, chart,
// illustration) or the older info.txt (Name, Composer, Charter, Level, Song, Chart, Picture), as
// "key: value" lines.
struct PhigrosInfo
{
    std::string Title;
    std::string Artist;
    std::string Charter;
    std::string Level;
    // Relative to the package.
    std::string MusicFile;
    std::string ChartFile;
    std::string Illustration;
};

PhigrosInfo ReadPhigrosInfo(std::string_view text);

} // namespace hyoshi::line
