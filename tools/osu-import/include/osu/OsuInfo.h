#pragma once

#include "core/Result.h"
#include "core/Time.h"
#include "rhythm/Chart.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace hyoshi::osu
{

// What a song list shows of an .osu file, read without building its notes, so much quicker than
// either importer: its details, filled in as the importers fill them, and its hit objects counted.
struct OsuInfo
{
    // Title, artist, charter (Creator), difficulty name (Version), preview, and OverallDifficulty,
    // as ImportOsuMania and ImportOsuStandard read them.
    rhythm::ChartMetadata Metadata;
    std::string AudioFile;
    // The background image from [Events], or empty.
    std::string BackgroundFile;
    // [Metadata]'s Tags and Source: words to search by.
    std::string Tags;
    std::string Source;
    // 0 osu!standard, 1 taiko, 2 catch, 3 mania.
    int Mode = 0;
    // CircleSize rounded: an osu!mania map's key count.
    int KeyCount = 0;
    uint32_t HitObjectCount = 0;
    // When the last hit object starts, or an osu!mania hold or a spinner ends (sliders' ends aren't
    // worked out).
    SongTimeUs EndUs = 0;
    // The first red line's tempo, or 0.
    double Bpm = 0.0;
};

// Fails only for a file with no hit objects, which neither importer takes either.
Result<OsuInfo> ReadOsuInfo(std::string_view osuText);

} // namespace hyoshi::osu
