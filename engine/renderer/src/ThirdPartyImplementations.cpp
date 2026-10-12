// stb_truetype's implementation, for TextRenderer, and stb_image's and stb_image_resize2's, for Image
// (PNG, JPEG, and GIF's first frame).
// NOLINTBEGIN
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
