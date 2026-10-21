#pragma once

#include "audio/AudioTypes.h"

#include "core/SeqLock.h"
#include "core/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::audio
{

class MusicStream;

enum class AudioCommandType : uint8_t
{
    SetMusic,
    PlayMusic,
    PauseMusic,
    SeekMusic,
    SetMusicRate,
    SetMusicLowPass,
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
    // SetMusic: the music, either Buffer or Stream. PlayVoice, ReleaseBuffer: the sound.
    const PcmBuffer* Buffer = nullptr;
    MusicStream* Stream = nullptr;
    // SeekMusic on a stream: the epoch of the blocks decoded from the target (MusicStream).
    int64_t StreamEpoch = 0;
    // SetMusicRate: music frames per frame rendered.
    double Rate = 1.0;
    // SetMusicLowPass: the cutoff in Hz, or 0 for none.
    float CutoffHz = 0.0f;
};

// The engine's custom mixer (DESIGN.md section 11.2), independent of any device so it can run
// offline in tests. The main thread submits commands; the audio thread calls Render, which
// applies them, publishes the clock snapshot, and mixes music and voices.
//
// Render is real-time safe: no allocation, locks, logging, asserts, or file access. Buffers and
// streams are owned by the main thread, which keeps them alive until CommandsApplied shows they are
// released. Streamed music plays once its stream has decoded the blocks for the current position;
// until then it holds where it is, and snapshots report it as not playing.
//
// At a rate other than 1, the music is resampled linearly, which shifts its pitch with its speed.
// A low-pass (SetMusicLowPass, two one-pole stages) muffles the music alone, before the voices mix
// in; off, it costs nothing.
//
// While the music plays, each callback measures its levels in octave bands (MusicLevels: a
// band-pass biquad each, on the music alone, its volume divided out) and keeps the last few
// callbacks' for the main thread, which asks for the frame it hears (GetMusicLevels).
class Mixer
{
public:
    static constexpr DspFrame NOT_SCHEDULED = -1;
    static constexpr double MIN_MUSIC_RATE = 0.1;
    static constexpr double MAX_MUSIC_RATE = 4.0;

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

    // Any thread. The levels of the callback that played musicFrame, among the last LEVEL_HISTORY;
    // none if none did, or the music hasn't played lately.
    std::optional<MusicLevels> GetMusicLevels(DspFrame musicFrame) const;

    // Callbacks' levels kept: at 48 kHz and 256 frames a callback, 85 ms of them, more than the
    // device's latency between rendering a frame and its being heard.
    static constexpr size_t LEVEL_HISTORY = 16;

private:
    struct LevelHistory
    {
        std::array<MusicLevels, LEVEL_HISTORY> Entries{};
    };
    // A band's band-pass (RBJ's, 0 dB at its middle, an octave wide), direct form I, and the sum of
    // its output's squares this callback.
    struct BandFilter
    {
        double B0 = 0.0;
        double B2 = 0.0;
        double A1 = 0.0;
        double A2 = 0.0;
        double X1 = 0.0;
        double X2 = 0.0;
        double Y1 = 0.0;
        double Y2 = 0.0;
    };

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
    DspFrame GetMusicFrameCount() const;
    // The music frames that rendering frameCount frames would play.
    DspFrame GetMusicFramesFor(DspFrame frameCount) const;
    DspFrame RenderMusic(float* output, DspFrame frameCount);
    DspFrame RenderMusicAtRate(float* output, DspFrame frameCount);
    void FilterMusic(float* output, DspFrame frameCount);
    void MeasureMusic(const float* output, DspFrame frameCount, DspFrame musicStart, DspFrame musicFrames);
    void SetMusicCursor(DspFrame frame);
    float GetBusGain(Bus bus) const;
    static void MixInto(float* output, const int16_t* source, uint32_t frameCount, float gain);

    const uint32_t sampleRate;
    SpscQueue<AudioCommand> commands;
    SeqLock<AudioClockSnapshot> snapshot;
    SeqLock<LevelHistory> levels;

    // Audio thread state.
    std::vector<Voice> voices;
    std::array<float, static_cast<size_t>(Bus::Count)> busVolumes{};
    const PcmBuffer* music = nullptr;
    MusicStream* stream = nullptr;
    DspFrame musicCursor = 0;
    // At a rate other than 1: how far past musicCursor the music is, in [0, 1), and how many frames
    // a stream still has to skip to reach musicCursor (stepped over at the end of its last block).
    double musicRate = 1.0;
    double musicPhase = 0.0;
    DspFrame musicSkip = 0;
    // The music's low-pass: each stage's coefficient (0 when off) and each stage's last output, by
    // channel.
    float musicLowPass = 0.0f;
    std::array<float, size_t{PcmBuffer::CHANNELS} * 2> musicLowPassState{};
    // The music's levels: each band's filter (no coefficients for one past the rate's reach), and
    // the history published, with where the next goes.
    std::array<BandFilter, MusicLevels::BAND_HZ.size()> bandFilters{};
    LevelHistory levelEntries{};
    size_t nextLevel = 0;
    bool isMusicPlaying = false;
    // The last callback advanced the music, with no seek, pause, or music change since.
    bool wasMusicAdvancing = false;
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
    std::atomic<uint64_t> musicUnderrunCount{0};

    // Main thread only.
    uint64_t droppedCommandCount = 0;
};

} // namespace hyoshi::audio
