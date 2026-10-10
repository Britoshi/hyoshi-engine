#pragma once

#include "audio/AudioTypes.h"

#include "core/Result.h"
#include "core/Time.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hyoshi::audio
{

// Decodes WAV, MP3, FLAC, or Ogg Vorbis from memory to stereo 16-bit PCM at sampleRate, converting
// channels and sample rate as needed. Slow for full songs; call it from a worker job, and stream
// music instead (MusicStream).
Result<PcmBuffer> DecodeAudio(std::span<const std::byte> encoded, uint32_t sampleRate);

// Decodes part of a file at a time, from any position, as stereo 16-bit PCM at the output rate.
// Only one thread may use it at a time.
class IStreamDecoder
{
public:
    virtual ~IStreamDecoder() = default;

    // The length in frames at the output rate, known when the decoder opens.
    virtual DspFrame GetFrameCount() const = 0;
    // Decodes up to frameCount frames into samples. Returns the frames written: fewer only at the
    // end of the file or on a decoding error.
    virtual uint32_t Read(int16_t* samples, uint32_t frameCount) = 0;
    // Decodes and discards up to frameCount frames, returning how many.
    virtual DspFrame Skip(DspFrame frameCount) = 0;
    // Whether Seek lands on the frame that decoding from the start reaches. Seek(0) always does;
    // without exact seeks, a position is reached by seeking to 0 and skipping.
    virtual bool HasExactSeeks() const = 0;
    virtual bool Seek(DspFrame frame) = 0;
};

// Opens WAV, MP3, FLAC, or Ogg Vorbis for streaming at sampleRate. Fast: it reads the headers.
// MP3s don't have exact seeks: miniaudio's land up to 26 ms off.
Result<std::unique_ptr<IStreamDecoder>> OpenStreamDecoder(std::vector<std::byte> encoded, uint32_t sampleRate);

} // namespace hyoshi::audio
