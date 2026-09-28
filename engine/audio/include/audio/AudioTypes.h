#pragma once

#include "core/Time.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hyoshi::audio
{

// Hashed asset ID of a loaded sound.
using SoundId = uint64_t;

// One playing instance of a sound.
using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND_HANDLE = 0;

enum class Bus : uint8_t
{
    Master,
    Music,
    HitSounds,
    Ui,
    Preview,
    Count
};

// Interleaved stereo 16-bit PCM, already at the device's sample rate so playback never resamples.
struct PcmBuffer
{
    static constexpr uint32_t CHANNELS = 2;

    std::vector<int16_t> Samples;
    uint32_t SampleRate = 0;

    DspFrame GetFrameCount() const
    {
        return static_cast<DspFrame>(Samples.size() / CHANNELS);
    }
};

// Published by the audio thread at the start of every callback (DESIGN.md section 9). All fields
// are 64-bit so it can be copied through a SeqLock.
struct AudioClockSnapshot
{
    // Frames handed to the device before this callback.
    DspFrame DeviceFrames = 0;
    // Host time when the callback started.
    HostTimeNs HostTime = 0;
    int64_t SampleRate = 0;
    // Music position of the first frame this callback renders.
    DspFrame MusicFrame = 0;
    // 1 while the music advances during this callback.
    int64_t IsMusicPlaying = 0;
    // Incremented by every seek and music change, so the song clock can tell a jump from drift.
    int64_t SeekGeneration = 0;
    // Commands applied so far, so the main thread knows when a released buffer is no longer read.
    int64_t CommandsApplied = 0;
    // Frames this callback renders.
    int64_t CallbackFrames = 0;
};

struct AudioConfig
{
    // Read at the start of each callback to timestamp the clock snapshot. Must be the same clock
    // as input timestamps (SDL_GetTicksNS).
    HostClockFn HostClock = nullptr;
    uint32_t PeriodMs = 10;
    uint32_t VoiceCount = 32;
};

struct AudioDeviceInfo
{
    std::string BackendName;
    std::string DeviceName;
    uint32_t SampleRate = 0;
    uint32_t DeviceSampleRate = 0;
    uint32_t PeriodFrames = 0;
    uint32_t PeriodCount = 0;
};

// Counters for the debug panel. Read on the main thread.
struct AudioStats
{
    uint64_t Callbacks = 0;
    // Longest time between two callbacks since the last ResetStats, a sign of glitches.
    HostTimeNs MaxCallbackGapNs = 0;
    uint32_t ActiveVoices = 0;
    // Voices that couldn't start because every voice was busy.
    uint64_t DroppedVoices = 0;
    // Scheduled voices dropped because the music had already passed their start frame.
    uint64_t LateVoices = 0;
    // Commands lost to a full command queue.
    uint64_t DroppedCommands = 0;
    uint32_t RouteChanges = 0;
};

} // namespace hyoshi::audio
