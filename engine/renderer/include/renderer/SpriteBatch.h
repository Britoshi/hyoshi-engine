#pragma once

#include "renderer/Camera2D.h"

#include "core/Result.h"
#include "rhi/IRenderDevice.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hyoshi::renderer
{

// 8-bit RGBA, authored in sRGB and written to the display as-is.
struct Color
{
    uint8_t R = 255;
    uint8_t G = 255;
    uint8_t B = 255;
    uint8_t A = 255;
};

constexpr Color WHITE{255, 255, 255, 255};

// The color with its alpha scaled by `alpha` (0 to 1).
constexpr Color WithAlpha(Color color, float alpha)
{
    const float clamped = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    color.A = static_cast<uint8_t>(clamped * static_cast<float>(color.A));
    return color;
}

struct Sprite
{
    glm::vec2 Center{0.0f};
    glm::vec2 Size{0.0f};
    // Radians, clockwise on screen (y points down).
    float Rotation = 0.0f;
    // u0, v0, u1, v1 within the texture.
    glm::vec4 UvRect{0.0f, 0.0f, 1.0f, 1.0f};
    Color Tint = WHITE;
    // Invalid means a solid color.
    rhi::TextureHandle Texture;
    // Lower layers draw first. Within a layer, sprites are grouped by texture, so sprites that must
    // overlap in a specific order belong in different layers, or in a layer that keeps its order
    // (SpriteBatch::KeepOrder).
    int32_t Layer = 0;
    // The texture's alpha is a signed distance field (glyphs from TextRenderer): the shape's edge
    // is at 0.5, drawn anti-aliased at any scale.
    bool IsDistanceField = false;
};

// Instanced quads (DESIGN.md section 10.3): one draw call per run of sprites sharing a layer and
// texture, with per-instance data streamed each frame.
class SpriteBatch
{
public:
    Result<void> Initialize(rhi::IRenderDevice& device);
    void Shutdown();

    void Begin(const Camera2D& camera);
    // Until End, sprites in this layer draw in the order they were submitted, whatever their
    // textures: painter's order for things drawn back to front, like overlapping notes each with
    // text on them. Each change of texture costs a draw call.
    void KeepOrder(int32_t layer);
    void Draw(const Sprite& sprite);
    void DrawRect(glm::vec2 topLeft, glm::vec2 size, Color color, int32_t layer = 0);
    // Sorts, uploads, and records the draws. Call inside a render pass.
    void End(rhi::ICommandList& commands);

    uint32_t GetLastSpriteCount() const
    {
        return lastSpriteCount;
    }

    uint32_t GetLastDrawCallCount() const
    {
        return lastDrawCallCount;
    }

private:
    // Matches SpriteInstance in shaders/Sprite.slang.
    struct Instance
    {
        glm::vec2 Center;
        glm::vec2 Size;
        float Angle;
        glm::vec4 UvRect;
        uint32_t Color;
        // Bit 0: IsDistanceField.
        uint32_t Flags;
    };

    struct Entry
    {
        int32_t Layer;
        rhi::TextureHandle Texture;
        // The texture's sort key, or 0 in a layer that keeps its order.
        uint64_t TextureKey;
        Instance Data;
    };

    Result<void> EnsureCapacity(uint32_t frame, size_t spriteCount);

    rhi::IRenderDevice* device = nullptr;
    rhi::PipelineHandle pipeline;
    rhi::SamplerHandle sampler;
    rhi::TextureHandle whiteTexture;

    std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> instanceBuffers{};
    std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT> instanceCapacities{};

    ClipTransform clipTransform;
    std::vector<Entry> entries;
    std::vector<int32_t> orderedLayers;
    std::vector<Instance> instances;
    uint32_t lastSpriteCount = 0;
    uint32_t lastDrawCallCount = 0;
};

} // namespace hyoshi::renderer
