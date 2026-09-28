// miniaudio's implementation, with stb_vorbis for Ogg Vorbis. miniaudio enables Vorbis decoding
// when the stb_vorbis header is included before it; the stb_vorbis implementation follows.
// NOLINTBEGIN
#if defined(_MSC_VER)
// MSVC's code generator warns inside stb_vorbis, and /external:W0 doesn't cover code generator
// warnings.
#pragma warning(disable : 4701)
#endif

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
// NOLINTEND
