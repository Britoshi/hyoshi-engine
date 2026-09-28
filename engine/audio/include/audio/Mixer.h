#pragma once

#include "audio/AudioTypes.h"

#include "core/SeqLock.h"
#include "core/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace hyoshi::audio
{

enum class AudioCommandType : uint8_t
{
    SetMusic,
    PlayMusic,
    PauseMusic,
    SeekMusic,
    PlayVoice,
    StopVoice,
    SetBusVolume,
    // Stops everything reading Buffer, after which the main thread may free it.
    ReleaseBuffer
};

struct AudioCommand
{
    AudioCommandType Type = AudioCommandType::PlayMusic;
    Bus TargetBus = Bus::Master;
    SoundHandle Handle = INVALID_SOUND_HANDLE;
    float Volume = 1.0f;
    // SeekMusic: the target. PlayVoice: the music frame to start at, or NOT_SCHEDULED.
    DspFrame Frame = 0;
    const PcmBuffer* Buffer = nullptr;
};

// The engine's custom mixer (DESIGN.md section 11.2), independent of any device so it can run
// offline in tests. The main thread submits commands; the audio thread calls Render, which
// applies them, publishes the clock snapshot, and mixes music and voices.
//
// Render is real-time safe: no allocation, locks, logging, asserts, or file access. Buffers are
// owned by the main thread, which keeps them alive until CommandsApplied shows they are released.
class Mixer
{
public:
    static constexpr DspFrame NOT_SCHEDULED = -1;

    Mixer(uint32_t sampleRate, uint32_t voiceCount, size_t commandCapacity = 1024);

    // Main thread. Returns false, dropping the command, when the queue is full.
    bool Submit(const AudioCommand& command);

    // Audio thread. Writes frameCount interleaved stereo float frames.
    void Render(float* output, uint32_t frameCount, HostTimeNs callbackTime);

    // Any thread.
    AudioClockSnapshot GetSnapshot() const
    {
        return snapshot.Load();
    }

    uint32_t GetSampleRate() const
    {
        return sampleRate;
    }

    AudioStats GetStats() const;
    void ResetStats();

private:
    struct Voice
    {
        const PcmBuffer* Buffer = nullptr;
        SoundHandle Handle = INVALID_SOUND_HANDLE;
        DspFrame Cursor = 0;
        DspFrame StartMusicFrame = NOT_SCHEDULED;
        Bus TargetBus = Bus::Master;
        float Volume = 1.0f;
    };

    void Apply(const AudioCommand& command);
    void CancelScheduledVoices();
    float GetBusGain(Bus bus) const;
    static void MixInto(float* output, const PcmBuffer& buffer, DspFrame sourceFrame, uint32_t frameCount, float gain);

    const uint32_t sampleRate;
    SpscQueue<AudioCommand> commands;
    SeqLock<AudioClockSnapshot> snapshot;

    // Audio thread state.
    std::vector<Voice> voices;
    std::array<float, static_cast<size_t>(Bus::Count)> busVolumes{};
    const PcmBuffer* music = nullptr;
    DspFrame musicCursor = 0;
    bool isMusicPlaying = false;
    int64_t seekGeneration = 0;
    int64_t commandsApplied = 0;
    DspFrame deviceFrames = 0;
    HostTimeNs lastCallbackTime = 0;

    // Written by the audio thread, read by the main thread.
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<int64_t> maxCallbackGapNs{0};
    std::atomic<uint32_t> activeVoiceCount{0};
    std::atomic<uint64_t> droppedVoiceCount{0};
    std::atomic<uint64_t> lateVoiceCount{0};

    // Main thread only.
    uint64_t droppedCommandCount = 0;
};

} // namespace hyoshi::audio
