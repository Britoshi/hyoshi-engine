#include "AsioOutput.h"

#include "audio/Asio.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

namespace hyoshi::audio
{

namespace asio
{

// The ASIO 2 driver interface, written from its documented binary interface rather than taken from
// Steinberg's SDK, whose licenses (its own, or the GPL) don't suit the engine's. Drivers are COM
// objects registered under HKLM\SOFTWARE\ASIO. ASIO's `long` is 32 bits on Windows, and the SDK
// packs its structures to 4 bytes there.
#pragma pack(push, 4)

using Bool = long;
using ErrorCode = long;

struct Samples
{
    unsigned long Hi;
    unsigned long Lo;
};

struct ChannelInfo
{
    long Channel;
    Bool IsInput;
    Bool IsActive;
    long ChannelGroup;
    long SampleType;
    std::array<char, 32> Name;
};

struct BufferInfo
{
    Bool IsInput;
    long ChannelNumber;
    std::array<void*, 2> Buffers;
};

// Passed to bufferSwitchTimeInfo; only handed back.
struct Time;

struct Callbacks
{
    void (*BufferSwitch)(long doubleBufferIndex, Bool directProcess);
    void (*SampleRateDidChange)(double sampleRate);
    long (*Message)(long selector, long value, void* message, double* option);
    Time* (*BufferSwitchTimeInfo)(Time* time, long doubleBufferIndex, Bool directProcess);
};

#pragma pack(pop)

// IASIO: its methods in vtable order after IUnknown's. The names are ours; only the order matters.
struct Driver : public IUnknown
{
    virtual Bool Init(void* systemHandle) = 0;
    virtual void GetDriverName(char* name) = 0;
    virtual long GetDriverVersion() = 0;
    virtual void GetErrorMessage(char* message) = 0;
    virtual ErrorCode Start() = 0;
    virtual ErrorCode Stop() = 0;
    virtual ErrorCode GetChannels(long* inputs, long* outputs) = 0;
    virtual ErrorCode GetLatencies(long* input, long* output) = 0;
    virtual ErrorCode GetBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) = 0;
    virtual ErrorCode CanSampleRate(double sampleRate) = 0;
    virtual ErrorCode GetSampleRate(double* sampleRate) = 0;
    virtual ErrorCode SetSampleRate(double sampleRate) = 0;
    virtual ErrorCode GetClockSources(void* clocks, long* count) = 0;
    virtual ErrorCode SetClockSource(long reference) = 0;
    virtual ErrorCode GetSamplePosition(Samples* position, Samples* systemTime) = 0;
    virtual ErrorCode GetChannelInfo(ChannelInfo* info) = 0;
    virtual ErrorCode CreateBuffers(BufferInfo* infos, long channelCount, long bufferSize, Callbacks* callbacks) = 0;
    virtual ErrorCode DisposeBuffers() = 0;
    virtual ErrorCode ControlPanel() = 0;
    virtual ErrorCode Future(long selector, void* option) = 0;
    virtual ErrorCode OutputReady() = 0;
};

constexpr ErrorCode OK = 0;
constexpr ErrorCode SUCCESS = 0x3f4847a0;

// asioMessage selectors.
constexpr long SELECTOR_SUPPORTED = 1;
constexpr long ENGINE_VERSION = 2;
constexpr long RESET_REQUEST = 3;
constexpr long BUFFER_SIZE_CHANGE = 4;
constexpr long RESYNC_REQUEST = 5;
constexpr long LATENCIES_CHANGED = 6;
constexpr long SUPPORTS_TIME_INFO = 7;

// Sample types, little-endian (Windows). The INT32_LSB_n ones hold n bits in the low end.
constexpr long INT16_LSB = 16;
constexpr long INT24_LSB = 17;
constexpr long INT32_LSB = 18;
constexpr long FLOAT32_LSB = 19;
constexpr long FLOAT64_LSB = 20;
constexpr long INT32_LSB_16 = 24;
constexpr long INT32_LSB_18 = 25;
constexpr long INT32_LSB_20 = 26;
constexpr long INT32_LSB_24 = 27;

bool IsOk(ErrorCode code)
{
    return code == OK || code == SUCCESS;
}

} // namespace asio

namespace
{

constexpr uint32_t CHANNEL_COUNT = 2;
constexpr wchar_t DRIVERS_KEY[] = L"SOFTWARE\\ASIO";

// The output the driver's callbacks go to; they carry no user data.
std::atomic<AsioOutput*> activeOutput{nullptr};

void OnBufferSwitch(long doubleBufferIndex, asio::Bool /*directProcess*/)
{
    if (AsioOutput* output = activeOutput.load(std::memory_order_acquire); output != nullptr)
    {
        output->OnBufferSwitch(static_cast<int32_t>(doubleBufferIndex));
    }
}

asio::Time* OnBufferSwitchTimeInfo(asio::Time* /*time*/, long doubleBufferIndex, asio::Bool directProcess)
{
    OnBufferSwitch(doubleBufferIndex, directProcess);
    return nullptr;
}

void OnSampleRateDidChange(double /*sampleRate*/)
{
    if (AsioOutput* output = activeOutput.load(std::memory_order_acquire); output != nullptr)
    {
        output->RequestReset();
    }
}

long OnMessage(long selector, long value, void* /*message*/, double* /*option*/)
{
    switch (selector)
    {
    case asio::SELECTOR_SUPPORTED:
        return value == asio::ENGINE_VERSION || value == asio::RESET_REQUEST || value == asio::BUFFER_SIZE_CHANGE ||
                       value == asio::RESYNC_REQUEST || value == asio::LATENCIES_CHANGED ||
                       value == asio::SUPPORTS_TIME_INFO
                   ? 1
                   : 0;
    case asio::ENGINE_VERSION:
        return 2;
    // A new buffer size or latency (the driver's control panel): make the buffers again.
    case asio::RESET_REQUEST:
    case asio::BUFFER_SIZE_CHANGE:
    case asio::LATENCIES_CHANGED:
        OnSampleRateDidChange(0.0);
        return 1;
    // Lost samples; nothing to redo, since the song clock follows the callbacks.
    case asio::RESYNC_REQUEST:
    case asio::SUPPORTS_TIME_INFO:
        return 1;
    default:
        return 0;
    }
}

asio::Callbacks callbacks{&OnBufferSwitch, &OnSampleRateDidChange, &OnMessage, &OnBufferSwitchTimeInfo};

std::optional<uint32_t> GetSampleBytes(long sampleType)
{
    switch (sampleType)
    {
    case asio::INT16_LSB:
        return 2;
    case asio::INT24_LSB:
        return 3;
    case asio::INT32_LSB:
    case asio::FLOAT32_LSB:
    case asio::INT32_LSB_16:
    case asio::INT32_LSB_18:
    case asio::INT32_LSB_20:
    case asio::INT32_LSB_24:
        return 4;
    case asio::FLOAT64_LSB:
        return 8;
    default:
        return std::nullopt;
    }
}

// One channel of interleaved stereo, clamped and scaled to fullScale.
int64_t ToInteger(float sample, double fullScale)
{
    return std::llrint(static_cast<double>(std::clamp(sample, -1.0f, 1.0f)) * fullScale);
}

template <typename T>
void WriteIntegers(const float* interleaved, uint32_t frameCount, void* output, double fullScale)
{
    auto* samples = static_cast<T*>(output);
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        samples[i] = static_cast<T>(ToInteger(interleaved[i * CHANNEL_COUNT], fullScale));
    }
}

void WriteChannel(const float* interleaved, uint32_t frameCount, long sampleType, void* output)
{
    switch (sampleType)
    {
    case asio::INT16_LSB:
        WriteIntegers<int16_t>(interleaved, frameCount, output, 32767.0);
        break;
    case asio::INT24_LSB:
    {
        auto* bytes = static_cast<uint8_t*>(output);
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            const auto value = static_cast<uint32_t>(ToInteger(interleaved[i * CHANNEL_COUNT], 8388607.0));
            bytes[(i * 3) + 0] = static_cast<uint8_t>(value);
            bytes[(i * 3) + 1] = static_cast<uint8_t>(value >> 8U);
            bytes[(i * 3) + 2] = static_cast<uint8_t>(value >> 16U);
        }
        break;
    }
    case asio::INT32_LSB:
        WriteIntegers<int32_t>(interleaved, frameCount, output, 2147483647.0);
        break;
    case asio::INT32_LSB_16:
        WriteIntegers<int32_t>(interleaved, frameCount, output, 32767.0);
        break;
    case asio::INT32_LSB_18:
        WriteIntegers<int32_t>(interleaved, frameCount, output, 131071.0);
        break;
    case asio::INT32_LSB_20:
        WriteIntegers<int32_t>(interleaved, frameCount, output, 524287.0);
        break;
    case asio::INT32_LSB_24:
        WriteIntegers<int32_t>(interleaved, frameCount, output, 8388607.0);
        break;
    case asio::FLOAT32_LSB:
    {
        auto* samples = static_cast<float*>(output);
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            samples[i] = interleaved[i * CHANNEL_COUNT];
        }
        break;
    }
    case asio::FLOAT64_LSB:
    {
        auto* samples = static_cast<double*>(output);
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            samples[i] = interleaved[i * CHANNEL_COUNT];
        }
        break;
    }
    default:
        break;
    }
}

std::string ToUtf8(std::wstring_view text)
{
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring ToWide(std::string_view text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::optional<CLSID> FindDriverClassId(const std::string& driverName)
{
    const std::wstring key = std::wstring(DRIVERS_KEY) + L"\\" + ToWide(driverName);
    std::array<wchar_t, 64> text{};
    auto size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, text.data(), &size) !=
        ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    CLSID classId{};
    if (FAILED(CLSIDFromString(text.data(), &classId)))
    {
        return std::nullopt;
    }
    return classId;
}

} // namespace

std::vector<std::string> ListAsioDrivers()
{
    std::vector<std::string> names;
    HKEY drivers = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DRIVERS_KEY, 0, KEY_READ, &drivers) != ERROR_SUCCESS)
    {
        return names;
    }
    std::array<wchar_t, 256> name{};
    for (DWORD index = 0;; ++index)
    {
        auto length = static_cast<DWORD>(name.size());
        if (RegEnumKeyExW(drivers, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        names.push_back(ToUtf8({name.data(), length}));
    }
    RegCloseKey(drivers);
    return names;
}

AsioOutput::~AsioOutput()
{
    Close();
}

Result<void> AsioOutput::Open(const std::string& name, RenderFn renderFn, void* user)
{
    if (driver != nullptr)
    {
        return Error{"An ASIO driver is already open"};
    }
    const std::optional<CLSID> classId = FindDriverClassId(name);
    if (!classId)
    {
        return Error{"No ASIO driver is installed as '" + name + "'"};
    }
    AsioOutput* none = nullptr;
    if (!activeOutput.compare_exchange_strong(none, this))
    {
        return Error{"Another ASIO driver is open"};
    }
    driverName = name;
    render = renderFn;
    renderUser = user;

    // ASIO drivers are in-process COM objects, which take their class ID as the interface's too.
    shouldUninitializeCom = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    void* instance = nullptr;
    if (FAILED(CoCreateInstance(*classId, nullptr, CLSCTX_INPROC_SERVER, *classId, &instance)) || instance == nullptr)
    {
        Close();
        return Error{"The ASIO driver '" + name + "' couldn't be loaded"};
    }
    driver = static_cast<asio::Driver*>(instance);

    // Drivers may post to the window they're given; the desktop's, as hosts without one of their
    // own at hand give.
    if (driver->Init(GetDesktopWindow()) == 0)
    {
        std::array<char, 128> message{};
        driver->GetErrorMessage(message.data());
        Close();
        return Error{std::format("The ASIO driver '{}' didn't start: {}", name, message.data())};
    }
    long inputs = 0;
    long outputs = 0;
    if (!asio::IsOk(driver->GetChannels(&inputs, &outputs)) || outputs < static_cast<long>(CHANNEL_COUNT))
    {
        Close();
        return Error{"The ASIO driver '" + name + "' has no stereo output"};
    }
    double rate = 0.0;
    if (!asio::IsOk(driver->GetSampleRate(&rate)) || rate < 1.0)
    {
        Close();
        return Error{"The ASIO driver '" + name + "' has no sample rate"};
    }
    sampleRate = static_cast<uint32_t>(std::lround(rate));
    if (Result<void> result = CreateBuffers(); !result)
    {
        Close();
        return result;
    }
    return {};
}

Result<void> AsioOutput::CreateBuffers()
{
    long minimum = 0;
    long maximum = 0;
    long preferred = 0;
    long granularity = 0;
    if (!asio::IsOk(driver->GetBufferSize(&minimum, &maximum, &preferred, &granularity)) || preferred <= 0)
    {
        return Error{"The ASIO driver has no buffer size"};
    }
    bufferFrames = static_cast<uint32_t>(preferred);

    for (uint32_t channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        asio::ChannelInfo info{};
        info.Channel = static_cast<long>(channel);
        if (!asio::IsOk(driver->GetChannelInfo(&info)))
        {
            return Error{std::format("The ASIO driver didn't describe output {}", channel + 1)};
        }
        if (!GetSampleBytes(info.SampleType))
        {
            return Error{std::format("The ASIO driver's output {} uses sample type {}, which isn't supported",
                                     channel + 1, info.SampleType)};
        }
        channelTypes[channel] = static_cast<int32_t>(info.SampleType);
    }

    std::array<asio::BufferInfo, CHANNEL_COUNT> infos{};
    for (uint32_t channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        infos[channel].ChannelNumber = static_cast<long>(channel);
    }
    scratch.assign(static_cast<size_t>(bufferFrames) * CHANNEL_COUNT, 0.0f);
    if (!asio::IsOk(driver->CreateBuffers(infos.data(), static_cast<long>(CHANNEL_COUNT), preferred, &callbacks)))
    {
        return Error{std::format("The ASIO driver couldn't make buffers of {} frames", preferred)};
    }
    areBuffersCreated = true;
    for (uint32_t channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        channelBuffers[channel] = infos[channel].Buffers;
        const uint32_t bytes = bufferFrames * *GetSampleBytes(channelTypes[channel]);
        for (void* half : channelBuffers[channel])
        {
            std::memset(half, 0, bytes);
        }
    }

    long inputLatency = 0;
    long outputLatency = 0;
    driver->GetLatencies(&inputLatency, &outputLatency);
    outputLatencyFrames = outputLatency > 0 ? static_cast<uint32_t>(outputLatency) : bufferFrames;
    // Drivers that take it are told each buffer is ready as soon as it's written, rather than at
    // the next switch.
    canSignalOutputReady = asio::IsOk(driver->OutputReady());
    return {};
}

void AsioOutput::DisposeBuffers()
{
    if (areBuffersCreated)
    {
        driver->DisposeBuffers();
        areBuffersCreated = false;
        channelBuffers = {};
    }
}

void AsioOutput::Close()
{
    Stop();
    if (driver != nullptr)
    {
        DisposeBuffers();
        driver->Release();
        driver = nullptr;
    }
    if (shouldUninitializeCom)
    {
        CoUninitialize();
        shouldUninitializeCom = false;
    }
    AsioOutput* self = this;
    activeOutput.compare_exchange_strong(self, nullptr);
    render = nullptr;
    renderUser = nullptr;
}

Result<void> AsioOutput::Start()
{
    if (driver == nullptr || !areBuffersCreated)
    {
        return Error{"No ASIO driver is open"};
    }
    if (!isStarted)
    {
        if (!asio::IsOk(driver->Start()))
        {
            return Error{"The ASIO driver '" + driverName + "' didn't start playing"};
        }
        isStarted = true;
    }
    return {};
}

void AsioOutput::Stop()
{
    // After stop returns, the driver calls back no more.
    if (isStarted)
    {
        driver->Stop();
        isStarted = false;
    }
}

bool AsioOutput::ConsumeResetRequest()
{
    return isResetRequested.exchange(false);
}

Result<void> AsioOutput::Reset()
{
    if (driver == nullptr)
    {
        return Error{"No ASIO driver is open"};
    }
    const bool wasStarted = isStarted;
    Stop();
    DisposeBuffers();
    // Everything is mixed at the rate the driver was opened at.
    double rate = 0.0;
    if (asio::IsOk(driver->GetSampleRate(&rate)) && std::lround(rate) != static_cast<long>(sampleRate) &&
        !asio::IsOk(driver->SetSampleRate(static_cast<double>(sampleRate))))
    {
        return Error{std::format("The ASIO driver moved to {:.0f} Hz and can't go back to {} Hz", rate, sampleRate)};
    }
    if (Result<void> result = CreateBuffers(); !result)
    {
        return result;
    }
    return wasStarted ? Start() : Result<void>{};
}

bool AsioOutput::ShowControlPanel()
{
    return driver != nullptr && asio::IsOk(driver->ControlPanel());
}

void AsioOutput::RequestReset()
{
    isResetRequested.store(true);
}

void AsioOutput::OnBufferSwitch(int32_t bufferIndex)
{
    const auto half = static_cast<size_t>(bufferIndex & 1);
    render(renderUser, scratch.data(), bufferFrames);
    for (uint32_t channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        WriteChannel(scratch.data() + channel, bufferFrames, channelTypes[channel], channelBuffers[channel][half]);
    }
    if (canSignalOutputReady)
    {
        driver->OutputReady();
    }
}

} // namespace hyoshi::audio
