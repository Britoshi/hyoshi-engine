// stb_truetype's implementation, for TextRenderer, and stb_image's and stb_image_resize2's, for Image
// (PNG, JPEG, and GIF's first frame).
// NOLINTBEGIN
// Optimized in debug builds too (MSVC's): unoptimized, decoding a storyboard's pictures takes seconds,
// and nobody steps through these. Optimizing needs MSVC's runtime checks off here.
#if defined(_MSC_VER) && !defined(__clang__) && !defined(NDEBUG)
#pragma runtime_checks("", off)
#pragma optimize("gt", on)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
// NOLINTEND
