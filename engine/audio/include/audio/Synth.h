#pragma once

#include "audio/AudioTypes.h"

#include <cstdint>

namespace hyoshi::audio
{

// Generated sounds for metronomes, calibration (DESIGN.md section 11.5), and tests.

// A decaying tone that starts at full amplitude on its first frame, so its onset is sample-exact.
PcmBuffer MakeClick(uint32_t sampleRate, float frequencyHz, float durationMs, float gain);

// Where beat `index` falls, with a constant tempo.
SongTimeUs GetBeatTimeUs(double bpm, SongTimeUs firstBeatUs, int64_t index);

// A click on every beat, higher pitched on the first beat of each bar, then one second of silence.
PcmBuffer MakeMetronomeTrack(uint32_t sampleRate, double bpm, uint32_t beatCount, uint32_t beatsPerBar,
                             SongTimeUs firstBeatUs);

} // namespace hyoshi::audio
