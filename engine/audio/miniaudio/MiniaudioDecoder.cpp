#include "audio/AudioDecoder.h"

#include <miniaudio.h>

#include <array>
#include <string>

namespace hyoshi::audio
{

namespace
{

constexpr ma_uint64 DECODE_CHUNK_FRAMES = 4096;

} // namespace

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
