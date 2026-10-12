#pragma once

#include "core/Time.h"

#include <cstdint>
#include <optional>
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
    // The pack (a collection of songs) the chart belongs to, for a game to group songs by; empty
    // for none.
    std::string Pack;
    // osu!'s overall difficulty (OD, 0 to 10), from an osu! import: how tight its hit windows are.
    // A game may judge by it or not.
    std::optional<double> OverallDifficulty;
};

// Notes due within this of another are due together (a chord): every mode's playfield makes them
// shine alike (renderer::SHINE), so a chord reads the same way in any of them.
constexpr SongTimeUs SIMULTANEOUS_US = 1'000;

// Marks each note of a list sorted by time that's due together with another (its IsSimultaneous).
// It only sets the mark, so marks from elsewhere (notes of another mode due with them) stay.
template <typename Note>
void MarkSimultaneous(std::vector<Note>& notes)
{
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const bool isWithPrevious = i > 0 && notes[i].Time - notes[i - 1].Time <= SIMULTANEOUS_US;
        const bool isWithNext = i + 1 < notes.size() && notes[i + 1].Time - notes[i].Time <= SIMULTANEOUS_US;
        notes[i].IsSimultaneous = notes[i].IsSimultaneous || isWithPrevious || isWithNext;
    }
}

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
    // Which of Chart::Modes the note belongs to.
    uint8_t Mode = 0;

    static constexpr int32_t NO_SAMPLE = -1;

    bool HasDuration() const
    {
        return EndTime > Time;
    }
};

struct Chart
{
    static constexpr int64_t FORMAT_VERSION = 1;
    // Format 2 holds notes of several modes in one chart (DESIGN.md section 13.5).
    static constexpr int64_t MIXED_FORMAT_VERSION = 2;
    static constexpr size_t MAX_MODES = 8;

    // The first of Modes.
    std::string Mode;
    // The kinds of notes the chart has, by mode name, as the file lists them: one in format 1.
    std::vector<std::string> Modes;
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

// The chart with only one mode's notes (Mode and Modes set to it), for that mode's runtime chart.
Chart SelectMode(const Chart& chart, uint8_t mode);

} // namespace hyoshi::rhythm
