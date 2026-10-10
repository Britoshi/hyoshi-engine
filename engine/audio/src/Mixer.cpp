#include "audio/Mixer.h"

#include "audio/MusicStream.h"

#include <algorithm>
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
        isMusicPlaying ? std::clamp<DspFrame>(GetMusicFrameCount() - musicCursor, 0, frameCount) : 0;
    const DspFrame musicReady = stream != nullptr ? stream->Collect() : musicWanted;
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
    snapshot.Store(current);

    std::fill_n(output, size_t{frameCount} * PcmBuffer::CHANNELS, 0.0f);

    const DspFrame musicStart = musicCursor;
    const DspFrame musicFramesRendered = isMusicAdvancing ? RenderMusic(output, musicWanted) : 0;
    if (wasMusicAdvancing && musicFramesRendered < musicWanted)
    {
        // The stream's decoder fell behind in the middle of playback.
        musicUnderrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    // Running short ends a stretch of playback, so one stall counts once.
    wasMusicAdvancing = isMusicAdvancing && musicFramesRendered == musicWanted;
    musicCursor += musicFramesRendered;
    if (musicCursor >= GetMusicFrameCount())
    {
        isMusicPlaying = false;
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
            if (relative >= musicFramesRendered)
            {
                ++activeVoices;
                continue;
            }
            startOffset = relative;
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
        musicCursor = 0;
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
        musicCursor = std::clamp<DspFrame>(command.Frame, 0, GetMusicFrameCount());
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
            musicCursor = 0;
            isMusicPlaying = false;
        }
        break;
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

// Mixes up to frameCount frames of music from the cursor, and returns how many there were: fewer
// than asked only if a stream ran out of decoded blocks.
DspFrame Mixer::RenderMusic(float* output, DspFrame frameCount)
{
    const float gain = GetBusGain(Bus::Music);
    if (music != nullptr)
    {
        MixInto(output, music->Samples.data() + (musicCursor * PcmBuffer::CHANNELS), static_cast<uint32_t>(frameCount),
                gain);
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
