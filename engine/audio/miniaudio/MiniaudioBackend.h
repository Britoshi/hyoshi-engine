#pragma once

#include "audio/IAudioBackend.h"
#include "audio/Mixer.h"

#include <miniaudio.h>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hyoshi::audio
{

// DESIGN.md section 11.2: a playback-only miniaudio device at the device's native rate, mixed by
// the engine's Mixer in the device callback. AAudio on Android, CoreAudio on Apple platforms.
class MiniaudioBackend final : public IAudioBackend
{
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    Result<void> Initialize(const AudioConfig& config) override;
    void Shutdown() override;
    void Update() override;
    void Suspend() override;
    void Resume() override;

    uint32_t GetSampleRate() const override;

    Result<void> LoadSound(SoundId sound, PcmBuffer pcm) override;
    SoundHandle Play(SoundId sound, Bus bus, float volume) override;
    SoundHandle PlayAtMusicFrame(SoundId sound, Bus bus, DspFrame musicFrame, float volume) override;
    void Stop(SoundHandle handle) override;
    void SetBusVolume(Bus bus, float volume) override;

    Result<int64_t> SetMusic(PcmBuffer pcm) override;
    void PlayMusic() override;
    void PauseMusic() override;
    int64_t SeekMusic(DspFrame frame) override;
    DspFrame GetMusicFrameCount() const override;

    AudioClockSnapshot GetClockSnapshot() const override;
    int64_t GetEstimatedOutputLatencyUs() const override;
    bool ConsumeRouteChangedFlag() override;

    AudioDeviceInfo GetDeviceInfo() const override;
    AudioStats GetStats() const override;
    void ResetStats() override;

    // Called by miniaudio.
    void OnData(float* output, uint32_t frameCount);
    void OnRouteChanged();

private:
    struct RetiredBuffer
    {
        std::unique_ptr<PcmBuffer> Buffer;
        // Freed once the audio thread has applied this many commands.
        int64_t RequiredCommands = 0;
    };

    bool Submit(const AudioCommand& command);
    Result<void> CheckFormat(const PcmBuffer& pcm) const;
    void Retire(std::unique_ptr<PcmBuffer> buffer);
    SoundHandle StartVoice(SoundId sound, Bus bus, DspFrame musicFrame, float volume);

    std::unique_ptr<ma_device> device;
    std::unique_ptr<Mixer> mixer;
    HostClockFn hostClock = nullptr;
    bool isDeviceStarted = false;

    // Main thread state.
    std::unordered_map<SoundId, std::unique_ptr<PcmBuffer>> sounds;
    std::unique_ptr<PcmBuffer> music;
    std::vector<RetiredBuffer> retiredBuffers;
    int64_t commandsSubmitted = 0;
    int64_t seekGeneration = 0;
    SoundHandle nextHandle = 1;

    std::atomic<bool> isRouteChanged{false};
    std::atomic<uint32_t> routeChangeCount{0};
};

} // namespace hyoshi::audio
