// stb_truetype's implementation, for TextRenderer, and stb_image's, for Image (PNG and JPEG only).
// NOLINTBEGIN
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
// NOLINTEND
