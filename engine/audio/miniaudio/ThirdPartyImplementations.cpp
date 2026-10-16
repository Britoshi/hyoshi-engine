// stb_vorbis's implementation, for miniaudio's Ogg Vorbis decoding. miniaudio's own implementation is
// in Mp3Backend.cpp, with stb_vorbis's declarations before it.
// NOLINTBEGIN
#if defined(_MSC_VER)
// MSVC's code generator warns inside stb_vorbis, and /external:W0 doesn't cover code generator
// warnings.
#pragma warning(disable : 4701)
#endif
// Optimized in debug builds too (the decoding is what's slow unoptimized, and nobody steps through
// it). Here for MSVC, which needs its runtime checks off for it; other compilers get -O2 for this
// file (CMakeLists.txt).
#if defined(_MSC_VER) && !defined(__clang__) && !defined(NDEBUG)
#pragma runtime_checks("", off)
#pragma optimize("gt", on)
#endif

#include <stb_vorbis.c>
// NOLINTEND
