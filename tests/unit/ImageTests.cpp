#include "renderer/Image.h"

#include <doctest/doctest.h>
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using hyoshi::renderer::Image;

namespace
{

// A transparent image with an opaque rectangle.
Image MakeRectangle(uint32_t width, uint32_t height, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom)
{
    Image image;
    image.Width = width;
    image.Height = height;
    image.Pixels.assign(size_t{width} * height * 4, 0);
    for (uint32_t y = top; y < bottom; ++y)
    {
        for (uint32_t x = left; x < right; ++x)
        {
            image.Pixels[((size_t{y} * width) + x) * 4 + 3] = 255;
        }
    }
    return image;
}

uint8_t At(const Image& field, uint32_t x, uint32_t y)
{
    return field.Pixels[(size_t{y} * field.Width) + x];
}

} // namespace

TEST_CASE("DecodeImage reads a PNG as RGBA")
{
    const std::vector<uint8_t> pixels{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 40};
    std::vector<std::byte> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size)
        {
            auto* out = static_cast<std::vector<std::byte>*>(context);
            const auto* bytes = static_cast<const std::byte*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &png, 2, 2, 4, pixels.data(), 8);
    REQUIRE_FALSE(png.empty());

    hyoshi::Result<Image> decoded = hyoshi::renderer::DecodeImage(png);
    REQUIRE(decoded);
    CHECK(decoded.Value().Width == 2);
    CHECK(decoded.Value().Height == 2);
    CHECK(decoded.Value().Channels == 4);
    CHECK(decoded.Value().Pixels == pixels);

    const std::vector<std::byte> garbage(64, std::byte{7});
    CHECK_FALSE(hyoshi::renderer::DecodeImage(garbage));
}

TEST_CASE("MakeDistanceField trims margins and puts the edge at 0.5")
{
    // A 40x30 rectangle in an 80x60 image. Trimming keeps 2 pixels around it (44x34); with 64
    // texels at most and 8 of spread, those 44 pixels get 48 texels.
    const Image field = hyoshi::renderer::MakeDistanceField(MakeRectangle(80, 60, 20, 15, 60, 45), 64, 8);
    REQUIRE(field.Channels == 1);
    CHECK(field.Width == 64);
    CHECK(field.Height == 53);
    CHECK(At(field, 32, 26) == 255);
    CHECK(At(field, 0, 0) == 0);

    // Along the middle row, the shape (0.5 and up) is centered and 40 * 48 / 44 texels wide.
    const uint32_t row = field.Height / 2;
    uint32_t left = 0;
    while (left < field.Width && At(field, left, row) < 128)
    {
        ++left;
    }
    uint32_t right = field.Width - 1;
    while (right > 0 && At(field, right, row) < 128)
    {
        --right;
    }
    CHECK(right - left + 1 >= 43);
    CHECK(right - left + 1 <= 45);
    CHECK(static_cast<int>(left) - static_cast<int>(field.Width - 1 - right) <= 1);
    CHECK(static_cast<int>(field.Width - 1 - right) - static_cast<int>(left) <= 1);
    // Just outside the edge the field falls below 0.5, but not to nothing.
    CHECK(At(field, left - 1, row) > 96);

    CHECK(hyoshi::renderer::MakeDistanceField(MakeRectangle(8, 8, 0, 0, 0, 0), 64, 8).IsEmpty());
}

TEST_CASE("MakeDistanceField ignores nearly transparent noise away from the shapes")
{
    // Two squares with a gap between them, and a faint (alpha 1) patch in the gap.
    Image image = MakeRectangle(160, 60, 10, 10, 50, 50);
    for (uint32_t y = 10; y < 50; ++y)
    {
        for (uint32_t x = 110; x < 150; ++x)
        {
            image.Pixels[((size_t{y} * 160) + x) * 4 + 3] = 255;
        }
        if (y >= 25 && y < 35)
        {
            for (uint32_t x = 75; x < 85; ++x)
            {
                image.Pixels[((size_t{y} * 160) + x) * 4 + 3] = 1;
            }
        }
    }
    const Image field = hyoshi::renderer::MakeDistanceField(image, 160, 8);
    // The gap's middle is far outside both squares.
    CHECK(At(field, field.Width / 2, field.Height / 2) < 32);
}

TEST_CASE("MakeDistanceField can drop stray specks")
{
    Image image = MakeRectangle(200, 100, 60, 20, 140, 80);
    // A 2x2 dot far to the left.
    for (const uint32_t y : {50u, 51u})
    {
        for (const uint32_t x : {5u, 6u})
        {
            image.Pixels[((size_t{y} * 200) + x) * 4 + 3] = 255;
        }
    }
    const Image withSpeck = hyoshi::renderer::MakeDistanceField(image, 128, 8);
    const Image withoutSpeck = hyoshi::renderer::MakeDistanceField(image, 128, 8, 0.01f);
    const Image clean = hyoshi::renderer::MakeDistanceField(MakeRectangle(200, 100, 60, 20, 140, 80), 128, 8);
    CHECK(withSpeck.Width == 128);
    // The speck widens the trimmed bounds, so the rectangle comes out shorter.
    CHECK(withSpeck.Height < clean.Height);
    CHECK(withoutSpeck.Width == clean.Width);
    CHECK(withoutSpeck.Height == clean.Height);
    CHECK(withoutSpeck.Pixels == clean.Pixels);
}

TEST_CASE("MakeDistanceField downsamples large artwork")
{
    const Image field = hyoshi::renderer::MakeDistanceField(MakeRectangle(2000, 1000, 100, 100, 1900, 700), 256, 8);
    CHECK(field.Width == 256);
    CHECK(field.Height < 256);
    CHECK(At(field, field.Width / 2, field.Height / 2) == 255);
    CHECK(At(field, 0, 0) == 0);
}
