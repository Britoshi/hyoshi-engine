#include "audio/AudioDecoder.h"

#include "Mp3Backend.h"

#include <miniaudio.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hyoshi::audio
{

namespace
{

constexpr ma_uint64 DECODE_CHUNK_FRAMES = 4096;

// Whether miniaudio's own decoding seeks this file to the exact frame sequential decoding reaches:
// WAV, AIFF, FLAC, and Ogg Vorbis. Its MP3 seeks don't, so anything else is offered the exact MP3
// backend first.
bool IsExactSeekFormat(std::span<const std::byte> encoded)
{
    if (encoded.size() < 4)
    {
        return false;
    }
    const std::string_view magic(reinterpret_cast<const char*>(encoded.data()), 4);
    return magic == "RIFF" || magic == "RIFX" || magic == "RF64" || magic == "riff" || magic == "FORM" ||
           magic == "fLaC" || magic == "OggS";
}

class MiniaudioStreamDecoder final : public IStreamDecoder
{
public:
    explicit MiniaudioStreamDecoder(std::vector<std::byte> encodedBytes) : encoded(std::move(encodedBytes))
    {
    }

    ~MiniaudioStreamDecoder() override
    {
        if (isOpen)
        {
            ma_decoder_uninit(&decoder);
        }
    }

    MiniaudioStreamDecoder(const MiniaudioStreamDecoder&) = delete;
    MiniaudioStreamDecoder& operator=(const MiniaudioStreamDecoder&) = delete;

    // The decoder reads from `encoded` and must stay at its address, so this object is opened in
    // place.
    Result<void> Open(uint32_t sampleRate)
    {
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, PcmBuffer::CHANNELS, sampleRate);
        // miniaudio tries custom backends first, then its own.
        std::array<ma_decoding_backend_vtable*, 1> mp3Backend{GetExactMp3Backend()};
        const bool isBuiltInExact = IsExactSeekFormat(encoded);
        if (!isBuiltInExact)
        {
            config.ppCustomBackendVTables = mp3Backend.data();
            config.customBackendCount = static_cast<ma_uint32>(mp3Backend.size());
        }
        if (const ma_result result = ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder);
            result != MA_SUCCESS)
        {
            return Error{std::string("Unsupported or corrupt audio: ") + ma_result_description(result)};
        }
        isOpen = true;
        hasExactSeeks = isBuiltInExact || decoder.pBackendVTable == GetExactMp3Backend();

        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS || length == 0)
        {
            // Formats that don't say: count by decoding once, without keeping the samples.
            length = 0;
            ma_uint64 framesRead = 0;
            do
            {
                framesRead = 0;
                ma_decoder_read_pcm_frames(&decoder, nullptr, DECODE_CHUNK_FRAMES, &framesRead);
                length += framesRead;
            } while (framesRead > 0);
            if (ma_decoder_seek_to_pcm_frame(&decoder, 0) != MA_SUCCESS)
            {
                return Error{"Could not rewind the audio after measuring it"};
            }
        }
        if (length == 0)
        {
            return Error{"Audio file contains no samples"};
        }
        frameCount = static_cast<DspFrame>(length);
        return {};
    }

    DspFrame GetFrameCount() const override
    {
        return frameCount;
    }

    uint32_t Read(int16_t* samples, uint32_t count) override
    {
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&decoder, samples, count, &framesRead);
        return static_cast<uint32_t>(framesRead);
    }

    DspFrame Skip(DspFrame count) override
    {
        ma_uint64 skipped = 0;
        ma_decoder_read_pcm_frames(&decoder, nullptr, static_cast<ma_uint64>(count), &skipped);
        return static_cast<DspFrame>(skipped);
    }

    bool HasExactSeeks() const override
    {
        return hasExactSeeks;
    }

    bool Seek(DspFrame frame) override
    {
        return ma_decoder_seek_to_pcm_frame(&decoder, static_cast<ma_uint64>(frame)) == MA_SUCCESS;
    }

private:
    std::vector<std::byte> encoded;
    ma_decoder decoder{};
    bool isOpen = false;
    bool hasExactSeeks = false;
    DspFrame frameCount = 0;
};

} // namespace

Result<std::unique_ptr<IStreamDecoder>> OpenStreamDecoder(std::vector<std::byte> encoded, uint32_t sampleRate)
{
    auto decoder = std::make_unique<MiniaudioStreamDecoder>(std::move(encoded));
    if (Result<void> result = decoder->Open(sampleRate); !result)
    {
        return result.GetError();
    }
    return std::unique_ptr<IStreamDecoder>(std::move(decoder));
}

Result<PcmBuffer> DecodeAudio(std::span<const std::byte> encoded, uint32_t sampleRate)
{
    const ma_decoder_config config = ma_decoder_config_init(ma_format_s16, PcmBuffer::CHANNELS, sampleRate);
    ma_decoder decoder;
    if (const ma_result result = ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder);
        result != MA_SUCCESS)
    {
        return Error{std::string("Unsupported or corrupt audio: ") + ma_result_description(result)};
    }

    PcmBuffer pcm;
    pcm.SampleRate = sampleRate;

    // Vorbis can't report its length up front; the reserve is only a hint.
    ma_uint64 lengthFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &lengthFrames) == MA_SUCCESS && lengthFrames > 0)
    {
        pcm.Samples.reserve(static_cast<size_t>(lengthFrames) * PcmBuffer::CHANNELS);
    }

    std::array<int16_t, DECODE_CHUNK_FRAMES * PcmBuffer::CHANNELS> chunk{};
    while (true)
    {
        ma_uint64 framesRead = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), DECODE_CHUNK_FRAMES, &framesRead);
        pcm.Samples.insert(pcm.Samples.end(), chunk.begin(),
                           chunk.begin() + static_cast<ptrdiff_t>(framesRead * PcmBuffer::CHANNELS));
        if (result == MA_AT_END || framesRead < DECODE_CHUNK_FRAMES)
        {
            break;
        }
        if (result != MA_SUCCESS)
        {
            ma_decoder_uninit(&decoder);
            return Error{std::string("Audio decoding failed: ") + ma_result_description(result)};
        }
    }

    ma_decoder_uninit(&decoder);
    if (pcm.Samples.empty())
    {
        return Error{"Audio file contains no samples"};
    }
    return pcm;
}

} // namespace hyoshi::audio
