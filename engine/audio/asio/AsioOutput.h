#pragma once

#include "core/Result.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hyoshi::audio
{

namespace asio
{
struct Driver;
}

// Plays through an ASIO driver (Windows): the first two output channels, at the driver's sample
// rate and its preferred buffer size (set in its control panel), filled by a render function on the
// driver's thread. ASIO's callbacks carry no user data, so only one can be open at a time. All
// methods are for the thread that opened it, which ASIO drivers expect.
class AsioOutput
{
public:
    // Fills frameCount interleaved stereo frames. Runs on the driver's thread.
    using RenderFn = void (*)(void* user, float* output, uint32_t frameCount);

    AsioOutput() = default;
    ~AsioOutput();
    AsioOutput(const AsioOutput&) = delete;
    AsioOutput& operator=(const AsioOutput&) = delete;

    // driverName: as ListAsioDrivers names it.
    Result<void> Open(const std::string& driverName, RenderFn render, void* user);
    void Close();
    Result<void> Start();
    void Stop();

    // True once after the driver asked to be set up again: its buffer size or sample rate changed
    // (in its control panel), or it lost sync. Reset does it.
    bool ConsumeResetRequest();
    // Makes the buffers again at the driver's new size, back at the sample rate it was opened at.
    Result<void> Reset();

    // Opens the driver's own settings window. False if it has none.
    bool ShowControlPanel();

    const std::string& GetDriverName() const
    {
        return driverName;
    }
    uint32_t GetSampleRate() const
    {
        return sampleRate;
    }
    uint32_t GetBufferFrames() const
    {
        return bufferFrames;
    }
    // Frames from a buffer switch until the buffer's first frame is heard, as the driver reports it.
    uint32_t GetOutputLatencyFrames() const
    {
        return outputLatencyFrames;
    }

    // Called by the driver.
    void OnBufferSwitch(int32_t bufferIndex);
    void RequestReset();

private:
    Result<void> CreateBuffers();
    void DisposeBuffers();

    asio::Driver* driver = nullptr;
    bool shouldUninitializeCom = false;
    bool areBuffersCreated = false;
    bool isStarted = false;
    bool canSignalOutputReady = false;
    std::string driverName;
    uint32_t sampleRate = 0;
    uint32_t bufferFrames = 0;
    uint32_t outputLatencyFrames = 0;
    RenderFn render = nullptr;
    void* renderUser = nullptr;

    // The two output channels' double buffers (ASIO's), and their sample formats.
    std::array<std::array<void*, 2>, 2> channelBuffers{};
    std::array<int32_t, 2> channelTypes{};
    // The render function's interleaved output.
    std::vector<float> scratch;
    std::atomic<bool> isResetRequested{false};
};

} // namespace hyoshi::audio
