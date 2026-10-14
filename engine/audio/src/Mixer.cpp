#include "audio/Mixer.h"

#include "audio/MusicStream.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace hyoshi::audio
{

namespace
{

constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;

} // namespace

Mixer::Mixer(uint32_t rate, uint32_t voiceCount, size_t commandCapacity)
    : sampleRate(rate), commands(commandCapacity), voices(voiceCount)
{
    busVolumes.fill(1.0f);

    AudioClockSnapshot initial;
    initial.SampleRate = rate;
    snapshot.Store(initial);
}

bool Mixer::Submit(const AudioCommand& command)
{
    if (!commands.Push(command))
    {
        ++droppedCommandCount;
        return false;
    }
    return true;
}

// Real-time safe: see the class comment.
void Mixer::Render(float* output, uint32_t frameCount, HostTimeNs callbackTime)
{
    while (const std::optional<AudioCommand> command = commands.Pop())
    {
        Apply(*command);
        ++commandsApplied;
    }

    // What the music would play this callback, and whether it can: streamed music holds until the
    // blocks for its position are decoded, and after holding, until a whole callback's worth is, so
    // it doesn't stutter as it starts. Collect also frees blocks from before a seek.
    const DspFrame musicWanted =
        isMusicPlaying ? std::clamp<DspFrame>(GetMusicFramesFor(frameCount), 0, GetMusicFrameCount() - musicCursor) : 0;
    const DspFrame musicReady = stream != nullptr ? stream->Collect() - musicSkip : musicWanted;
    const bool isMusicAdvancing = musicWanted > 0 && musicReady >= (wasMusicAdvancing ? 1 : musicWanted);

    AudioClockSnapshot current;
    current.DeviceFrames = deviceFrames;
    current.HostTime = callbackTime;
    current.SampleRate = sampleRate;
    current.MusicFrame = musicCursor;
    current.IsMusicPlaying = isMusicAdvancing ? 1 : 0;
    current.SeekGeneration = seekGeneration;
    current.CommandsApplied = commandsApplied;
    current.CallbackFrames = frameCount;
    current.MusicRate = musicRate;
    snapshot.Store(current);

    std::fill_n(output, size_t{frameCount} * PcmBuffer::CHANNELS, 0.0f);

    const DspFrame musicStart = musicCursor;
    const double musicStartPhase = musicPhase;
    const DspFrame rendered = isMusicAdvancing ? RenderMusic(output, frameCount) : 0;
    const DspFrame musicFramesPlayed = musicCursor - musicStart;
    // Short of the callback before the music's end: the stream's decoder fell behind.
    const bool isShort = musicWanted > 0 && rendered < frameCount && musicCursor < GetMusicFrameCount();
    if (wasMusicAdvancing && isShort)
    {
        musicUnderrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    // Running short ends a stretch of playback, so one stall counts once.
    wasMusicAdvancing = isMusicAdvancing && !isShort;
    if (musicCursor >= GetMusicFrameCount())
    {
        isMusicPlaying = false;
    }
    // The low-pass on the music alone: the voices mix in after.
    if (musicLowPass > 0.0f)
    {
        FilterMusic(output, frameCount);
    }

    uint32_t activeVoices = 0;
    for (Voice& voice : voices)
    {
        if (voice.Buffer == nullptr)
        {
            continue;
        }

        // A scheduled voice starts at the output frame where the music reaches its start frame.
        // If the music already passed it (the main thread stalled), it is dropped: a click at the
        // wrong time is worse than a missing one.
        DspFrame startOffset = 0;
        if (voice.StartMusicFrame != NOT_SCHEDULED)
        {
            const DspFrame relative = voice.StartMusicFrame - musicStart;
            if (relative < 0)
            {
                lateVoiceCount.fetch_add(1, std::memory_order_relaxed);
                voice = Voice{};
                continue;
            }
            if (relative >= musicFramesPlayed)
            {
                ++activeVoices;
                continue;
            }
            startOffset = relative;
            if (musicRate != 1.0 || musicStartPhase != 0.0)
            {
                // The first frame rendered at or past the start frame.
                const double offset = std::ceil((static_cast<double>(relative) - musicStartPhase) / musicRate);
                startOffset = std::clamp<DspFrame>(static_cast<DspFrame>(offset), 0, rendered - 1);
            }
            voice.StartMusicFrame = NOT_SCHEDULED;
        }

        const DspFrame count = std::min(voice.Buffer->GetFrameCount() - voice.Cursor, frameCount - startOffset);
        MixInto(output + (startOffset * PcmBuffer::CHANNELS),
                voice.Buffer->Samples.data() + (voice.Cursor * PcmBuffer::CHANNELS), static_cast<uint32_t>(count),
                voice.Volume * GetBusGain(voice.TargetBus));
        voice.Cursor += count;
        if (voice.Cursor >= voice.Buffer->GetFrameCount())
        {
            voice = Voice{};
        }
        else
        {
            ++activeVoices;
        }
    }

    if (lastCallbackTime != 0)
    {
        const HostTimeNs gap = callbackTime - lastCallbackTime;
        if (gap > maxCallbackGapNs.load(std::memory_order_relaxed))
        {
            maxCallbackGapNs.store(gap, std::memory_order_relaxed);
        }
    }
    lastCallbackTime = callbackTime;
    deviceFrames += frameCount;
    callbackCount.fetch_add(1, std::memory_order_relaxed);
    activeVoiceCount.store(activeVoices, std::memory_order_relaxed);
}

void Mixer::Apply(const AudioCommand& command)
{
    switch (command.Type)
    {
    case AudioCommandType::SetMusic:
        music = command.Buffer;
        stream = command.Stream;
        SetMusicCursor(0);
        musicRate = 1.0;
        musicLowPass = 0.0f;
        isMusicPlaying = false;
        wasMusicAdvancing = false;
        ++seekGeneration;
        CancelScheduledVoices();
        break;
    case AudioCommandType::PlayMusic:
        isMusicPlaying = musicCursor < GetMusicFrameCount();
        break;
    case AudioCommandType::PauseMusic:
        isMusicPlaying = false;
        wasMusicAdvancing = false;
        break;
    case AudioCommandType::SeekMusic:
        // The generation changes even without music, so the main thread can predict it.
        SetMusicCursor(std::clamp<DspFrame>(command.Frame, 0, GetMusicFrameCount()));
        if (musicCursor >= GetMusicFrameCount())
        {
            isMusicPlaying = false;
        }
        if (stream != nullptr)
        {
            stream->SetReadEpoch(command.StreamEpoch);
        }
        wasMusicAdvancing = false;
        ++seekGeneration;
        CancelScheduledVoices();
        break;
    case AudioCommandType::SetMusicRate:
        musicRate = std::isfinite(command.Rate) ? std::clamp(command.Rate, MIN_MUSIC_RATE, MAX_MUSIC_RATE) : 1.0;
        if (musicRate == 1.0)
        {
            // Back on whole frames, a fraction of a frame on.
            musicPhase = 0.0;
        }
        break;
    case AudioCommandType::SetMusicLowPass:
    {
        // Off at 0, or at a cutoff too high to change anything.
        const float cutoff = std::isfinite(command.CutoffHz) ? command.CutoffHz : 0.0f;
        const auto rate = static_cast<float>(sampleRate);
        const float coefficient = cutoff > 0.0f && cutoff < rate * 0.45f
                                      ? 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * cutoff / rate)
                                      : 0.0f;
        if (musicLowPass == 0.0f)
        {
            musicLowPassState.fill(0.0f);
        }
        musicLowPass = coefficient;
        break;
    }
    case AudioCommandType::PlayVoice:
    {
        auto freeVoice = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return v.Buffer == nullptr; });
        if (freeVoice == voices.end() || command.Buffer == nullptr)
        {
            droppedVoiceCount.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        freeVoice->Buffer = command.Buffer;
        freeVoice->Handle = command.Handle;
        freeVoice->Cursor = 0;
        freeVoice->StartMusicFrame = command.Frame;
        freeVoice->TargetBus = command.TargetBus;
        freeVoice->Volume = command.Volume;
        break;
    }
    case AudioCommandType::StopVoice:
        for (Voice& voice : voices)
        {
            if (voice.Handle == command.Handle)
            {
                voice = Voice{};
            }
        }
        break;
    case AudioCommandType::SetBusVolume:
        busVolumes[static_cast<size_t>(command.TargetBus)] = command.Volume;
        break;
    case AudioCommandType::ReleaseBuffer:
        for (Voice& voice : voices)
        {
            if (voice.Buffer == command.Buffer)
            {
                voice = Voice{};
            }
        }
        if (music == command.Buffer)
        {
            music = nullptr;
            SetMusicCursor(0);
            isMusicPlaying = false;
        }
        break;
    }
}

void Mixer::FilterMusic(float* output, DspFrame frameCount)
{
    const size_t samples = static_cast<size_t>(frameCount) * PcmBuffer::CHANNELS;
    for (size_t i = 0; i < samples; ++i)
    {
        const size_t channel = i % PcmBuffer::CHANNELS;
        float& first = musicLowPassState[channel];
        float& second = musicLowPassState[PcmBuffer::CHANNELS + channel];
        first += musicLowPass * (output[i] - first);
        second += musicLowPass * (first - second);
        output[i] = second;
    }
}

// Scheduled start frames refer to the old music position, so a seek invalidates them.
void Mixer::CancelScheduledVoices()
{
    for (Voice& voice : voices)
    {
        if (voice.Buffer != nullptr && voice.StartMusicFrame != NOT_SCHEDULED)
        {
            voice = Voice{};
        }
    }
}

DspFrame Mixer::GetMusicFrameCount() const
{
    if (music != nullptr)
    {
        return music->GetFrameCount();
    }
    return stream != nullptr ? stream->GetFrameCount() : 0;
}

DspFrame Mixer::GetMusicFramesFor(DspFrame frameCount) const
{
    if (musicRate == 1.0 && musicPhase == 0.0)
    {
        return frameCount;
    }
    return static_cast<DspFrame>(std::ceil(musicPhase + (static_cast<double>(frameCount) * musicRate)));
}

void Mixer::SetMusicCursor(DspFrame frame)
{
    musicCursor = frame;
    musicPhase = 0.0;
    musicSkip = 0;
}

// Mixes up to frameCount frames of music from the cursor, moves the cursor past what they played,
// and returns how many it mixed: fewer than asked only at the end of the music, or if a stream ran
// out of decoded blocks.
DspFrame Mixer::RenderMusic(float* output, DspFrame frameCount)
{
    if (musicRate != 1.0 || musicPhase != 0.0 || musicSkip != 0)
    {
        return RenderMusicAtRate(output, frameCount);
    }

    frameCount = std::min(frameCount, GetMusicFrameCount() - musicCursor);
    const float gain = GetBusGain(Bus::Music);
    if (music != nullptr)
    {
        MixInto(output, music->Samples.data() + (musicCursor * PcmBuffer::CHANNELS), static_cast<uint32_t>(frameCount),
                gain);
        musicCursor += frameCount;
        return frameCount;
    }

    DspFrame rendered = 0;
    while (rendered < frameCount)
    {
        const std::span<const int16_t> ready = stream->Peek(static_cast<uint32_t>(frameCount - rendered));
        if (ready.empty())
        {
            break;
        }
        const auto frames = static_cast<uint32_t>(ready.size() / PcmBuffer::CHANNELS);
        MixInto(output + (rendered * PcmBuffer::CHANNELS), ready.data(), frames, gain);
        stream->Consume(frames);
        rendered += frames;
    }
    musicCursor += rendered;
    return rendered;
}

// Each frame rendered interpolates between the music frames either side of the position, which
// moves musicRate frames per frame. A stream comes a block at a time, so on a block's last frame it
// holds that frame rather than interpolate toward the next block's first, which is inaudible.
DspFrame Mixer::RenderMusicAtRate(float* output, DspFrame frameCount)
{
    const float scale = GetBusGain(Bus::Music) * INT16_TO_FLOAT;
    const DspFrame end = GetMusicFrameCount();
    DspFrame rendered = 0;
    while (rendered < frameCount && musicCursor < end)
    {
        std::span<const int16_t> ready;
        if (music != nullptr)
        {
            ready = std::span<const int16_t>(music->Samples)
                        .subspan(static_cast<size_t>(musicCursor) * PcmBuffer::CHANNELS);
        }
        else
        {
            while (musicSkip > 0)
            {
                const std::span<const int16_t> skipped =
                    stream->Peek(static_cast<uint32_t>(std::min<DspFrame>(musicSkip, MusicStream::BLOCK_FRAMES)));
                if (skipped.empty())
                {
                    break;
                }
                const auto frames = static_cast<uint32_t>(skipped.size() / PcmBuffer::CHANNELS);
                stream->Consume(frames);
                musicSkip -= frames;
            }
            if (musicSkip > 0)
            {
                break;
            }
            ready = stream->Peek(MusicStream::BLOCK_FRAMES);
        }
        if (ready.empty())
        {
            break;
        }

        const auto readyFrames = static_cast<DspFrame>(ready.size() / PcmBuffer::CHANNELS);
        DspFrame index = 0;
        while (rendered < frameCount && index < readyFrames)
        {
            const int16_t* from = ready.data() + (index * PcmBuffer::CHANNELS);
            const int16_t* to = index + 1 < readyFrames ? from + PcmBuffer::CHANNELS : from;
            const auto blend = static_cast<float>(musicPhase);
            float* target = output + (rendered * PcmBuffer::CHANNELS);
            for (size_t channel = 0; channel < PcmBuffer::CHANNELS; ++channel)
            {
                const auto a = static_cast<float>(from[channel]);
                const auto b = static_cast<float>(to[channel]);
                target[channel] += (a + ((b - a) * blend)) * scale;
            }
            ++rendered;
            musicPhase += musicRate;
            const double step = std::floor(musicPhase);
            musicPhase -= step;
            index += static_cast<DspFrame>(step);
        }
        if (stream != nullptr)
        {
            const DspFrame consumed = std::min(index, readyFrames);
            stream->Consume(static_cast<uint32_t>(consumed));
            musicSkip = index - consumed;
        }
        musicCursor = std::min(musicCursor + index, end);
    }
    return rendered;
}

float Mixer::GetBusGain(Bus bus) const
{
    const float master = busVolumes[static_cast<size_t>(Bus::Master)];
    return bus == Bus::Master ? master : master * busVolumes[static_cast<size_t>(bus)];
}

void Mixer::MixInto(float* output, const int16_t* source, uint32_t frameCount, float gain)
{
    const float scale = gain * INT16_TO_FLOAT;
    const size_t sampleCount = size_t{frameCount} * PcmBuffer::CHANNELS;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        output[i] += static_cast<float>(source[i]) * scale;
    }
}

AudioStats Mixer::GetStats() const
{
    AudioStats stats;
    stats.Callbacks = callbackCount.load(std::memory_order_relaxed);
    stats.MaxCallbackGapNs = maxCallbackGapNs.load(std::memory_order_relaxed);
    stats.ActiveVoices = activeVoiceCount.load(std::memory_order_relaxed);
    stats.DroppedVoices = droppedVoiceCount.load(std::memory_order_relaxed);
    stats.LateVoices = lateVoiceCount.load(std::memory_order_relaxed);
    stats.MusicUnderruns = musicUnderrunCount.load(std::memory_order_relaxed);
    stats.DroppedCommands = droppedCommandCount;
    return stats;
}

// Racing with the audio thread can lose a reset or an increment, which is fine for debug counters.
void Mixer::ResetStats()
{
    maxCallbackGapNs.store(0, std::memory_order_relaxed);
    droppedVoiceCount.store(0, std::memory_order_relaxed);
    lateVoiceCount.store(0, std::memory_order_relaxed);
    musicUnderrunCount.store(0, std::memory_order_relaxed);
    droppedCommandCount = 0;
}

} // namespace hyoshi::audio
