// stb_vorbis's implementation, for miniaudio's Ogg Vorbis decoding. miniaudio's own implementation is
// in Mp3Backend.cpp, with stb_vorbis's declarations before it.
// NOLINTBEGIN
#if defined(_MSC_VER)
// MSVC's code generator warns inside stb_vorbis, and /external:W0 doesn't cover code generator
// warnings.
#pragma warning(disable : 4701)
#endif

#include <stb_vorbis.c>
// NOLINTEND
