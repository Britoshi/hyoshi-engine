#pragma once

#include "audio/AudioTypes.h"

#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace hyoshi::audio
{

// Decodes WAV, MP3, FLAC, or Ogg Vorbis from memory to stereo 16-bit PCM at sampleRate, converting
// channels and sample rate as needed. Slow for full songs; call it from a worker job.
Result<PcmBuffer> DecodeAudio(std::span<const std::byte> encoded, uint32_t sampleRate);

} // namespace hyoshi::audio
