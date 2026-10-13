#pragma once

#include <miniaudio.h>

namespace hyoshi::audio
{

// miniaudio's MP3 decoding, for files in memory, with seeks that land on the frame decoding from the
// start reaches, as WAV, FLAC, and Ogg Vorbis seeks do. miniaudio's own MP3 seeks land up to two MP3
// frames (52 ms) late or a little early. For ma_decoder_config::ppCustomBackendVTables.
ma_decoding_backend_vtable* GetExactMp3Backend();

} // namespace hyoshi::audio
