#pragma once

#include "ui/Ui.h"

#include "rhi/IRenderDevice.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>

namespace hyoshi::ui
{

// Artwork from an image's alpha (a logo, an icon), kept as a single-channel distance field so one
// texture draws sharp at any size, in any tint.
struct Logo
{
    rhi::TextureHandle Texture;
    // The texture's size, including Spread texels of field around the artwork.
    glm::vec2 Size{0.0f};
    float Spread = 0.0f;

    bool IsLoaded() const
    {
        return Texture.IsValid();
    }

    // Width over height of the artwork itself.
    float GetAspect() const;
};

// Loads a PNG or JPEG (an asset path on Android) as a Logo at most maxSize texels on its long
// side. Logs and returns an unloaded Logo on failure.
Logo LoadLogo(rhi::IRenderDevice& device, const std::string& path, uint32_t maxSize);
void DestroyLogo(rhi::IRenderDevice& device, Logo& logo);

// Draws the artwork as large as fits in `area`, centered, and returns the rectangle it covers.
Rect DrawLogo(Ui& ui, const Logo& logo, const Rect& area, Color tint, int32_t layerOffset = 1);

} // namespace hyoshi::ui
