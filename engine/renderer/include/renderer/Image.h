#pragma once

#include "core/Result.h"
#include "rhi/IRenderDevice.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hyoshi::renderer
{

// Pixels in memory, rows top to bottom: RGBA8 (4 channels) or one 8-bit channel.
struct Image
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Channels = 4;
    std::vector<uint8_t> Pixels;

    bool IsEmpty() const
    {
        return Width == 0 || Height == 0;
    }
};

// Decodes a PNG or JPEG file's contents to RGBA8.
Result<Image> DecodeImage(std::span<const std::byte> data);

// A single-channel signed distance field of an RGBA image's alpha, for drawing one-color artwork
// (logos, icons) crisply at any size as a distance field sprite, tinted any color. Transparent
// margins are trimmed. The longer side becomes at most maxSize texels, including `spread` texels
// of field around the shape; 0.5 is the edge, as in TextRenderer's glyphs. Detached specks smaller
// than speckFraction of the largest shape (stray pixels in the artwork) are dropped.
Image MakeDistanceField(const Image& image, uint32_t maxSize, uint32_t spread, float speckFraction = 0.0f);

// Uploads an RGBA8 or single-channel image as a texture. Single-channel textures read as white
// with the value in alpha (RhiTypes.h).
Result<rhi::TextureHandle> CreateTexture(rhi::IRenderDevice& device, const Image& image, const char* debugName);

} // namespace hyoshi::renderer
