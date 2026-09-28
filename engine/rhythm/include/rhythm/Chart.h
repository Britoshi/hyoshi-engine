#pragma once

#include "core/Time.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hyoshi::rhythm
{

// The mode-independent part of a chart (DESIGN.md section 13.2). All times are integer
// microseconds from the start of the audio file. Mode-specific note fields live in the mode.

struct ChartMetadata
{
    std::string Title;
    std::string Artist;
    std::string Charter;
    std::string DifficultyName;
    double DifficultyValue = 0.0;
    SongTimeUs PreviewStartUs = 0;
};

struct TimingPoint
{
    SongTimeUs Time = 0;
    double Bpm = 120.0;
    uint32_t BeatsPerBar = 4;
    uint32_t BeatUnit = 4;
};

struct ScrollVelocityPoint
{
    SongTimeUs Time = 0;
    double Multiplier = 1.0;
};

struct ChartNote
{
    SongTimeUs Time = 0;
    // Equals Time for notes without a duration.
    SongTimeUs EndTime = 0;
    // The note's own sound (keysound), an index into Chart::Samples, or NO_SAMPLE. It sounds at
    // Time whether or not the note is hit.
    int32_t Sample = NO_SAMPLE;
    // Percent, 0 to 100.
    uint32_t SampleVolume = 100;

    static constexpr int32_t NO_SAMPLE = -1;

    bool HasDuration() const
    {
        return EndTime > Time;
    }
};

struct Chart
{
    static constexpr int64_t FORMAT_VERSION = 1;

    std::string Mode;
    ChartMetadata Metadata;
    // Relative to the chart file.
    std::string AudioFile;
    // The chart offset (DESIGN.md section 9): corrects a chart authored against shifted audio.
    int64_t AudioOffsetUs = 0;
    // Sound files the notes refer to, relative to the chart file.
    std::vector<std::string> Samples;
    std::vector<TimingPoint> Timing;
    std::vector<ScrollVelocityPoint> ScrollVelocity;
    // Sorted by Time.
    std::vector<ChartNote> Notes;
};

} // namespace hyoshi::rhythm
