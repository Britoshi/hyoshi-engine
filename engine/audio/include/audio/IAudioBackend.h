#pragma once

#include "audio/AudioTypes.h"

#include "core/Result.h"

#include <memory>

namespace hyoshi::audio
{

// DESIGN.md section 11.1. All methods are for the main thread. Music is a single track whose
// position is the song clock's timeline; hit sounds are preloaded and played on a voice pool.
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    virtual Result<void> Initialize(const AudioConfig& config) = 0;
    virtual void Shutdown() = 0;

    // Once per frame: frees buffers the audio thread has released.
    virtual void Update() = 0;

    // Stops and restarts the device, for the app going to the background and back.
    virtual void Suspend() = 0;
    virtual void Resume() = 0;

    // Sounds and music must be decoded at this rate.
    virtual uint32_t GetSampleRate() const = 0;

    // Sounds. Loading an ID that exists replaces it.
    virtual Result<void> LoadSound(SoundId sound, PcmBuffer pcm) = 0;
    virtual SoundHandle Play(SoundId sound, Bus bus, float volume = 1.0f) = 0;
    // Starts the sound exactly when the music reaches musicFrame, sample-accurate. Waits while
    // the music is paused, and is cancelled by a seek or a music change. Dropped if the music has
    // already passed musicFrame when the audio thread receives it, so schedule ahead.
    virtual SoundHandle PlayAtMusicFrame(SoundId sound, Bus bus, DspFrame musicFrame, float volume = 1.0f) = 0;
    virtual void Stop(SoundHandle handle) = 0;
    virtual void SetBusVolume(Bus bus, float volume) = 0;

    // Music. SetMusic stops playback and rewinds to the start. Like SeekMusic, it returns the
    // SeekGeneration that snapshots will report once the audio thread applies it.
    virtual Result<int64_t> SetMusic(PcmBuffer pcm) = 0;
    virtual void PlayMusic() = 0;
    virtual void PauseMusic() = 0;
    // Returns the SeekGeneration that snapshots will report once the audio thread applies it.
    virtual int64_t SeekMusic(DspFrame frame) = 0;
    virtual DspFrame GetMusicFrameCount() const = 0;

    // Timing contract for the rhythm core.
    virtual AudioClockSnapshot GetClockSnapshot() const = 0;
    virtual int64_t GetEstimatedOutputLatencyUs() const = 0;

    // True once after the output route changes (headphones, Bluetooth) or an interruption.
    virtual bool ConsumeRouteChangedFlag() = 0;

    virtual AudioDeviceInfo GetDeviceInfo() const = 0;
    virtual AudioStats GetStats() const = 0;
    virtual void ResetStats() = 0;
};

std::unique_ptr<IAudioBackend> CreateAudioBackend();

} // namespace hyoshi::audio
