#include "MiniaudioBackend.h"

#include "core/Log.h"

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

} // namespace

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

    // Sample rate 0 keeps the device's native rate, so the OS doesn't resample (and add latency).
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = PcmBuffer::CHANNELS;
    deviceConfig.sampleRate = 0;
    deviceConfig.periodSizeInMilliseconds = config.PeriodMs;
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
    mixer = std::make_unique<Mixer>(device->sampleRate, config.VoiceCount);

    if (const ma_result result = ma_device_start(device.get()); result != MA_SUCCESS)
    {
        ma_device_uninit(device.get());
        device.reset();
        mixer.reset();
        return Error{std::string("Could not start the audio device: ") + ma_result_description(result)};
    }
    isDeviceStarted = true;

    const AudioDeviceInfo info = GetDeviceInfo();
    HYOSHI_LOG_INFO("Audio: {} '{}', {} Hz (device {} Hz), period {} frames x {}, ~{:.1f} ms output latency",
                    info.BackendName, info.DeviceName, info.SampleRate, info.DeviceSampleRate, info.PeriodFrames,
                    info.PeriodCount, static_cast<double>(GetEstimatedOutputLatencyUs()) / 1000.0);
    return {};
}

void MiniaudioBackend::Shutdown()
{
    if (device)
    {
        // Uninit stops the device and waits for the callback, after which every buffer is free.
        ma_device_uninit(device.get());
        device.reset();
    }
    isDeviceStarted = false;
    mixer.reset();
    retiredBuffers.clear();
    sounds.clear();
    music.reset();
}

void MiniaudioBackend::Update()
{
    if (!mixer)
    {
        return;
    }

    const int64_t applied = mixer->GetSnapshot().CommandsApplied;
    std::erase_if(retiredBuffers,
                  [applied](const RetiredBuffer& retired) { return retired.RequiredCommands <= applied; });
}

void MiniaudioBackend::Suspend()
{
    if (device && isDeviceStarted)
    {
        ma_device_stop(device.get());
        isDeviceStarted = false;
    }
}

void MiniaudioBackend::Resume()
{
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
    if (music)
    {
        Retire(std::move(music));
    }
    music = std::move(buffer);
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
    if (Submit({.Type = AudioCommandType::SeekMusic, .Frame = frame}))
    {
        ++seekGeneration;
    }
    return seekGeneration;
}

DspFrame MiniaudioBackend::GetMusicFrameCount() const
{
    return music ? music->GetFrameCount() : 0;
}

AudioClockSnapshot MiniaudioBackend::GetClockSnapshot() const
{
    return mixer ? mixer->GetSnapshot() : AudioClockSnapshot{};
}

// Baseline estimate from DESIGN.md section 11.4: the device's buffering. Calibration covers the
// rest.
int64_t MiniaudioBackend::GetEstimatedOutputLatencyUs() const
{
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
    retiredBuffers.push_back({std::move(buffer), commandsSubmitted});
}

std::unique_ptr<IAudioBackend> CreateAudioBackend()
{
    return std::make_unique<MiniaudioBackend>();
}

} // namespace hyoshi::audio
