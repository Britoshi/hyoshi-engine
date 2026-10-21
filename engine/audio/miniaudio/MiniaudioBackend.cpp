#include "MiniaudioBackend.h"

#include "audio/Asio.h"
#include "core/Log.h"

#if defined(_WIN32)
#include "AsioOutput.h"
#endif

#include <algorithm>
#include <string>
#include <utility>

namespace hyoshi::audio
{

namespace
{

void DataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount)
{
    static_cast<MiniaudioBackend*>(device->pUserData)->OnData(static_cast<float*>(output), frameCount);
}

// Can run on any thread; only sets atomics.
void NotificationCallback(const ma_device_notification* notification)
{
    switch (notification->type)
    {
    case ma_device_notification_type_rerouted:
    case ma_device_notification_type_interruption_began:
    case ma_device_notification_type_interruption_ended:
        static_cast<MiniaudioBackend*>(notification->pDevice->pUserData)->OnRouteChanged();
        break;
    default:
        break;
    }
}

#if defined(_WIN32)
void RenderAsio(void* backend, float* output, uint32_t frameCount)
{
    static_cast<MiniaudioBackend*>(backend)->OnData(output, frameCount);
}
#endif

} // namespace

#if !defined(_WIN32)
std::vector<std::string> ListAsioDrivers()
{
    return {};
}
#endif

MiniaudioBackend::MiniaudioBackend() = default;

MiniaudioBackend::~MiniaudioBackend()
{
    Shutdown();
}

Result<void> MiniaudioBackend::Initialize(const AudioConfig& config)
{
    if (config.HostClock == nullptr)
    {
        return Error{"AudioConfig::HostClock is required"};
    }
    hostClock = config.HostClock;
    periodMs = config.PeriodMs;
    voiceCount = config.VoiceCount;

    if (!config.AsioDriver.empty())
    {
        Result<void> result = OpenAsio(config.AsioDriver);
        if (result)
        {
            return {};
        }
        fallbackReason = result.GetError().Message;
        HYOSHI_LOG_WARN("Audio: {}; playing through the system's device instead", fallbackReason);
    }
    return OpenDevice();
}

Result<void> MiniaudioBackend::Reopen(const std::string& asioDriver)
{
    if (!mixer)
    {
        return Error{"Audio isn't open"};
    }
    std::string previous;
#if defined(_WIN32)
    if (asio)
    {
        previous = asio->GetDriverName();
    }
#endif
    CloseOutput();
    fallbackReason.clear();
    Result<void> opened = OpenOutput(asioDriver);
    if (!opened)
    {
        fallbackReason = opened.GetError().Message;
        HYOSHI_LOG_WARN("Audio: {}; back to the output before", fallbackReason);
        Result<void> back = OpenOutput(previous);
        if (!back && !previous.empty())
        {
            back = OpenOutput({});
        }
        if (!back)
        {
            HYOSHI_LOG_ERROR("Audio: no output to play through: {}", back.GetError().Message);
        }
    }
    // Song clocks take the new output's latency (SongPlayer).
    OnRouteChanged();
    return opened;
}

Result<void> MiniaudioBackend::OpenOutput(const std::string& asioDriver)
{
    return asioDriver.empty() ? OpenDevice() : OpenAsio(asioDriver);
}

Result<void> MiniaudioBackend::OpenDevice()
{
    // At first, sample rate 0 keeps the device's native rate, so the OS doesn't resample (and add
    // latency). Reopened, the mixer's rate, which everything loaded is at.
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = PcmBuffer::CHANNELS;
    deviceConfig.sampleRate = mixer ? mixer->GetSampleRate() : 0;
    deviceConfig.periodSizeInMilliseconds = periodMs;
    deviceConfig.performanceProfile = ma_performance_profile_low_latency;
    deviceConfig.dataCallback = DataCallback;
    deviceConfig.notificationCallback = NotificationCallback;
    deviceConfig.pUserData = this;

    device = std::make_unique<ma_device>();
    if (const ma_result result = ma_device_init(nullptr, &deviceConfig, device.get()); result != MA_SUCCESS)
    {
        device.reset();
        return Error{std::string("Could not open the audio device: ") + ma_result_description(result)};
    }

    // The mixer must exist before the first callback, which can come as soon as the device starts.
    const bool isNewMixer = mixer == nullptr;
    if (isNewMixer)
    {
        mixer = std::make_unique<Mixer>(device->sampleRate, voiceCount);
    }

    if (const ma_result result = ma_device_start(device.get()); result != MA_SUCCESS)
    {
        ma_device_uninit(device.get());
        device.reset();
        if (isNewMixer)
        {
            mixer.reset();
        }
        return Error{std::string("Could not start the audio device: ") + ma_result_description(result)};
    }
    isDeviceStarted = true;

    const AudioDeviceInfo info = GetDeviceInfo();
    HYOSHI_LOG_INFO("Audio: {} '{}', {} Hz (device {} Hz), period {} frames x {}, ~{:.1f} ms output latency",
                    info.BackendName, info.DeviceName, info.SampleRate, info.DeviceSampleRate, info.PeriodFrames,
                    info.PeriodCount, static_cast<double>(GetEstimatedOutputLatencyUs()) / 1000.0);
    return {};
}

Result<void> MiniaudioBackend::OpenAsio(const std::string& driverName)
{
#if defined(_WIN32)
    asio = std::make_unique<AsioOutput>();
    // Reopened, at the mixer's rate; at first, the driver's own.
    if (Result<void> result = asio->Open(driverName, &RenderAsio, this, mixer ? mixer->GetSampleRate() : 0); !result)
    {
        asio.reset();
        return result;
    }
    // As with a device: the mixer before the first callback.
    const bool isNewMixer = mixer == nullptr;
    if (isNewMixer)
    {
        mixer = std::make_unique<Mixer>(asio->GetSampleRate(), voiceCount);
    }
    if (Result<void> result = asio->Start(); !result)
    {
        asio.reset();
        if (isNewMixer)
        {
            mixer.reset();
        }
        return result;
    }
    isDeviceStarted = true;
    const AudioDeviceInfo info = GetDeviceInfo();
    HYOSHI_LOG_INFO("Audio: ASIO '{}', {} Hz, buffer {} frames, ~{:.1f} ms output latency", info.DeviceName,
                    info.SampleRate, info.PeriodFrames, static_cast<double>(GetEstimatedOutputLatencyUs()) / 1000.0);
    return {};
#else
    return Error{"ASIO is only on Windows, not for '" + driverName + "'"};
#endif
}

void MiniaudioBackend::CloseOutput()
{
#if defined(_WIN32)
    // Closing stops the driver, after which it calls back no more.
    asio.reset();
#endif
    if (device)
    {
        // Uninit stops the device and waits for the callback, after which every buffer is free.
        ma_device_uninit(device.get());
        device.reset();
    }
    isDeviceStarted = false;
}

void MiniaudioBackend::Shutdown()
{
    CloseOutput();
    mixer.reset();
    retiredBuffers.clear();
    sounds.clear();
    music.reset();
    musicStream.reset();
}

void MiniaudioBackend::Update()
{
    if (!mixer)
    {
        return;
    }
#if defined(_WIN32)
    // A new buffer size from the driver's control panel, for one.
    if (asio && asio->ConsumeResetRequest())
    {
        if (Result<void> result = asio->Reset(); result)
        {
            HYOSHI_LOG_INFO("Audio: ASIO reset, buffer {} frames, ~{:.1f} ms output latency", asio->GetBufferFrames(),
                            static_cast<double>(GetEstimatedOutputLatencyUs()) / 1000.0);
        }
        else
        {
            HYOSHI_LOG_ERROR("Audio: ASIO couldn't reset: {}", result.GetError().Message);
        }
    }
    // Only its latencies: the music plays on, the song clock told of the new one.
    if (asio && asio->ConsumeLatencyChange())
    {
        asio->ReadLatencies();
        isRouteChanged.store(true);
        HYOSHI_LOG_INFO("Audio: ASIO latencies changed, ~{:.1f} ms output latency",
                        static_cast<double>(GetEstimatedOutputLatencyUs()) / 1000.0);
    }
#endif

    const int64_t applied = mixer->GetSnapshot().CommandsApplied;
    std::erase_if(retiredBuffers,
                  [applied](const RetiredBuffer& retired) { return retired.RequiredCommands <= applied; });
}

void MiniaudioBackend::Suspend()
{
#if defined(_WIN32)
    if (asio)
    {
        asio->Stop();
        isDeviceStarted = false;
        return;
    }
#endif
    if (device && isDeviceStarted)
    {
        ma_device_stop(device.get());
        isDeviceStarted = false;
    }
}

void MiniaudioBackend::Resume()
{
#if defined(_WIN32)
    if (asio)
    {
        if (Result<void> result = asio->Start(); result)
        {
            isDeviceStarted = true;
        }
        else
        {
            HYOSHI_LOG_ERROR("Could not restart ASIO: {}", result.GetError().Message);
        }
        return;
    }
#endif
    if (device && !isDeviceStarted)
    {
        if (const ma_result result = ma_device_start(device.get()); result != MA_SUCCESS)
        {
            HYOSHI_LOG_ERROR("Could not restart the audio device: {}", ma_result_description(result));
            return;
        }
        isDeviceStarted = true;
    }
}

uint32_t MiniaudioBackend::GetSampleRate() const
{
    return mixer ? mixer->GetSampleRate() : 0;
}

Result<void> MiniaudioBackend::LoadSound(SoundId sound, PcmBuffer pcm)
{
    if (Result<void> result = CheckFormat(pcm); !result)
    {
        return result;
    }

    auto buffer = std::make_unique<PcmBuffer>(std::move(pcm));
    std::unique_ptr<PcmBuffer>& slot = sounds[sound];
    if (slot)
    {
        Submit({.Type = AudioCommandType::ReleaseBuffer, .Buffer = slot.get()});
        Retire(std::move(slot));
    }
    slot = std::move(buffer);
    return {};
}

SoundHandle MiniaudioBackend::Play(SoundId sound, Bus bus, float volume)
{
    return StartVoice(sound, bus, Mixer::NOT_SCHEDULED, volume);
}

SoundHandle MiniaudioBackend::PlayAtMusicFrame(SoundId sound, Bus bus, DspFrame musicFrame, float volume)
{
    return StartVoice(sound, bus, std::max<DspFrame>(musicFrame, 0), volume);
}

SoundHandle MiniaudioBackend::StartVoice(SoundId sound, Bus bus, DspFrame musicFrame, float volume)
{
    const auto found = sounds.find(sound);
    if (found == sounds.end() || !mixer)
    {
        HYOSHI_LOG_WARN("Audio: sound {:#x} is not loaded", sound);
        return INVALID_SOUND_HANDLE;
    }

    const SoundHandle handle = nextHandle;
    nextHandle = nextHandle == UINT32_MAX ? 1 : nextHandle + 1;
    const bool isQueued = Submit({.Type = AudioCommandType::PlayVoice,
                                  .TargetBus = bus,
                                  .Handle = handle,
                                  .Volume = volume,
                                  .Frame = musicFrame,
                                  .Buffer = found->second.get()});
    return isQueued ? handle : INVALID_SOUND_HANDLE;
}

void MiniaudioBackend::Stop(SoundHandle handle)
{
    Submit({.Type = AudioCommandType::StopVoice, .Handle = handle});
}

void MiniaudioBackend::SetBusVolume(Bus bus, float volume)
{
    Submit({.Type = AudioCommandType::SetBusVolume, .TargetBus = bus, .Volume = volume});
}

Result<int64_t> MiniaudioBackend::SetMusic(PcmBuffer pcm)
{
    if (Result<void> result = CheckFormat(pcm); !result)
    {
        return result.GetError();
    }

    auto buffer = std::make_unique<PcmBuffer>(std::move(pcm));
    if (!Submit({.Type = AudioCommandType::SetMusic, .Buffer = buffer.get()}))
    {
        return Error{"Audio command queue is full"};
    }
    ++seekGeneration;
    RetireMusic();
    music = std::move(buffer);
    return seekGeneration;
}

Result<int64_t> MiniaudioBackend::SetMusicStream(std::unique_ptr<MusicStream> stream)
{
    if (!mixer)
    {
        return Error{"The audio backend is not initialized"};
    }
    if (stream == nullptr || stream->GetSampleRate() != mixer->GetSampleRate())
    {
        return Error{"The music stream must decode at " + std::to_string(mixer->GetSampleRate()) + " Hz"};
    }
    if (!Submit({.Type = AudioCommandType::SetMusic, .Stream = stream.get()}))
    {
        return Error{"Audio command queue is full"};
    }
    ++seekGeneration;
    RetireMusic();
    musicStream = std::move(stream);
    streamEpoch = 0;
    return seekGeneration;
}

void MiniaudioBackend::PlayMusic()
{
    Submit({.Type = AudioCommandType::PlayMusic});
}

void MiniaudioBackend::PauseMusic()
{
    Submit({.Type = AudioCommandType::PauseMusic});
}

int64_t MiniaudioBackend::SeekMusic(DspFrame frame)
{
    const DspFrame target = std::clamp<DspFrame>(frame, 0, GetMusicFrameCount());
    const int64_t epoch = streamEpoch + 1;
    if (Submit({.Type = AudioCommandType::SeekMusic, .Frame = target, .StreamEpoch = epoch}))
    {
        ++seekGeneration;
        if (musicStream)
        {
            streamEpoch = epoch;
            musicStream->RequestSeek(target, epoch);
        }
    }
    return seekGeneration;
}

DspFrame MiniaudioBackend::GetMusicFrameCount() const
{
    if (music)
    {
        return music->GetFrameCount();
    }
    return musicStream ? musicStream->GetFrameCount() : 0;
}

void MiniaudioBackend::SetMusicRate(double rate)
{
    Submit({.Type = AudioCommandType::SetMusicRate, .Rate = rate});
}

void MiniaudioBackend::SetMusicLowPass(float cutoffHz)
{
    Submit({.Type = AudioCommandType::SetMusicLowPass, .CutoffHz = cutoffHz});
}

AudioClockSnapshot MiniaudioBackend::GetClockSnapshot() const
{
    return mixer ? mixer->GetSnapshot() : AudioClockSnapshot{};
}

std::optional<MusicLevels> MiniaudioBackend::GetMusicLevels(DspFrame musicFrame) const
{
    return mixer ? mixer->GetMusicLevels(musicFrame) : std::nullopt;
}

// Baseline estimate from DESIGN.md section 11.4: the device's buffering. Calibration covers the
// rest.
int64_t MiniaudioBackend::GetEstimatedOutputLatencyUs() const
{
#if defined(_WIN32)
    // ASIO drivers report theirs: from a buffer switch until its first frame is heard.
    if (asio)
    {
        return static_cast<int64_t>(asio->GetOutputLatencyFrames()) * US_PER_SECOND / asio->GetSampleRate();
    }
#endif
    if (!device || device->playback.internalSampleRate == 0)
    {
        return 0;
    }
    const auto bufferedFrames =
        static_cast<int64_t>(device->playback.internalPeriodSizeInFrames) * device->playback.internalPeriods;
    return bufferedFrames * US_PER_SECOND / device->playback.internalSampleRate;
}

bool MiniaudioBackend::ConsumeRouteChangedFlag()
{
    return isRouteChanged.exchange(false);
}

AudioDeviceInfo MiniaudioBackend::GetDeviceInfo() const
{
    AudioDeviceInfo info;
    info.FallbackReason = fallbackReason;
#if defined(_WIN32)
    if (asio)
    {
        info.BackendName = "ASIO";
        info.DeviceName = asio->GetDriverName();
        info.SampleRate = asio->GetSampleRate();
        info.DeviceSampleRate = asio->GetSampleRate();
        info.PeriodFrames = asio->GetBufferFrames();
        info.PeriodCount = 2;
        return info;
    }
#endif
    if (!device)
    {
        return info;
    }
    info.BackendName = ma_get_backend_name(device->pContext->backend);
    info.DeviceName = device->playback.name;
    info.SampleRate = device->sampleRate;
    info.DeviceSampleRate = device->playback.internalSampleRate;
    info.PeriodFrames = device->playback.internalPeriodSizeInFrames;
    info.PeriodCount = device->playback.internalPeriods;
    return info;
}

bool MiniaudioBackend::ShowOutputSettings()
{
#if defined(_WIN32)
    return asio && asio->ShowControlPanel();
#else
    return false;
#endif
}

AudioStats MiniaudioBackend::GetStats() const
{
    AudioStats stats = mixer ? mixer->GetStats() : AudioStats{};
    stats.RouteChanges = routeChangeCount.load();
    return stats;
}

void MiniaudioBackend::ResetStats()
{
    if (mixer)
    {
        mixer->ResetStats();
    }
}

void MiniaudioBackend::OnData(float* output, uint32_t frameCount)
{
    mixer->Render(output, frameCount, hostClock());
}

void MiniaudioBackend::OnRouteChanged()
{
    routeChangeCount.fetch_add(1);
    isRouteChanged.store(true);
}

bool MiniaudioBackend::Submit(const AudioCommand& command)
{
    if (!mixer || !mixer->Submit(command))
    {
        HYOSHI_LOG_ERROR("Audio: command queue full, dropped a command");
        return false;
    }
    ++commandsSubmitted;
    return true;
}

Result<void> MiniaudioBackend::CheckFormat(const PcmBuffer& pcm) const
{
    if (!mixer)
    {
        return Error{"The audio backend is not initialized"};
    }
    if (pcm.SampleRate != mixer->GetSampleRate() || pcm.Samples.size() % PcmBuffer::CHANNELS != 0)
    {
        return Error{"PCM must be interleaved stereo at " + std::to_string(mixer->GetSampleRate()) + " Hz, got " +
                     std::to_string(pcm.SampleRate) + " Hz"};
    }
    return {};
}

// The audio thread may still read the buffer until it applies the command just submitted.
void MiniaudioBackend::Retire(std::unique_ptr<PcmBuffer> buffer)
{
    retiredBuffers.push_back({.Buffer = std::move(buffer), .RequiredCommands = commandsSubmitted});
}

// After the command that replaces the music.
void MiniaudioBackend::RetireMusic()
{
    if (music)
    {
        Retire(std::move(music));
    }
    if (musicStream)
    {
        retiredBuffers.push_back({.Stream = std::move(musicStream), .RequiredCommands = commandsSubmitted});
    }
}

std::unique_ptr<IAudioBackend> CreateAudioBackend()
{
    return std::make_unique<MiniaudioBackend>();
}

} // namespace hyoshi::audio
