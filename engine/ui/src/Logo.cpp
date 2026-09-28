#include "ui/Logo.h"

#include "core/Log.h"
#include "platform/Platform.h"
#include "renderer/Image.h"

#include <algorithm>

namespace hyoshi::ui
{

namespace
{

// Texels of distance field around the artwork: enough to keep edges smooth when drawn larger
// than the texture.
constexpr uint32_t SPREAD = 12;
// Stray dots in the artwork, much smaller than its shapes, are left out.
constexpr float SPECK_FRACTION = 0.005f;

} // namespace

float Logo::GetAspect() const
{
    const glm::vec2 content = Size - (2.0f * Spread);
    return content.y > 0.0f ? content.x / content.y : 1.0f;
}

Logo LoadLogo(rhi::IRenderDevice& device, const std::string& path, uint32_t maxSize)
{
    Result<std::vector<std::byte>> bytes = platform::LoadFile(path);
    if (!bytes)
    {
        HYOSHI_LOG_WARN("Logo: {}", bytes.GetError().Message);
        return {};
    }
    Result<renderer::Image> image = renderer::DecodeImage(bytes.Value());
    if (!image)
    {
        HYOSHI_LOG_WARN("Logo: {}: {}", path, image.GetError().Message);
        return {};
    }
    const renderer::Image field = renderer::MakeDistanceField(image.Value(), maxSize, SPREAD, SPECK_FRACTION);
    Result<rhi::TextureHandle> texture = renderer::CreateTexture(device, field, "Logo");
    if (!texture)
    {
        HYOSHI_LOG_WARN("Logo: {}: {}", path, texture.GetError().Message);
        return {};
    }
    Logo logo;
    logo.Texture = texture.Value();
    logo.Size = {static_cast<float>(field.Width), static_cast<float>(field.Height)};
    logo.Spread = static_cast<float>(SPREAD);
    return logo;
}

void DestroyLogo(rhi::IRenderDevice& device, Logo& logo)
{
    device.Destroy(logo.Texture);
    logo = {};
}

Rect DrawLogo(Ui& ui, const Logo& logo, const Rect& area, Color tint, int32_t layerOffset)
{
    if (!logo.IsLoaded() || area.Width() <= 0.0f || area.Height() <= 0.0f)
    {
        return {area.Center(), area.Center()};
    }
    // Fit the artwork, not the texture: the field's margin may overhang the area.
    const glm::vec2 content = logo.Size - (2.0f * logo.Spread);
    const float scale = std::min(area.Width() / content.x, area.Height() / content.y);
    renderer::Sprite sprite;
    sprite.Center = area.Center();
    sprite.Size = logo.Size * scale;
    sprite.Texture = logo.Texture;
    sprite.Tint = tint;
    sprite.Layer = ui.GetLayer() + layerOffset;
    sprite.IsDistanceField = true;
    ui.GetSprites().Draw(sprite);
    const glm::vec2 half = content * scale * 0.5f;
    return {area.Center() - half, area.Center() + half};
}

} // namespace hyoshi::ui
