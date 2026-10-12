#pragma once

#include "renderer/Camera2D.h"

#include "core/Result.h"
#include "rhi/IRenderDevice.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
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
// The glow every mode puts behind notes due together with another (rhythm::SIMULTANEOUS_US).
constexpr Color SHINE{255, 222, 110, 255};

// An RGB color (each channel 0 to 1), opaque, mixed toward white until its relative luminance is
// at least minLuminance (0 to 1), so it reads on a dark background: a map's own colors, say.
Color ToLightColor(glm::vec3 rgb, float minLuminance);

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
    // Until End or the next call, fades every sprite drawn by `opacity` (0 to 1, from 1 at Begin):
    // a group, like a playfield, fading in or out together. Each sprite fades on its own, so
    // overlapping ones show through each other as they fade.
    void SetOpacity(float opacity);
    // Until End or the next call, shows every sprite drawn only inside `min` to `max` (virtual
    // units), as a playfield shown in part of the screen keeps to its part; ClearClip shows all.
    void SetClip(glm::vec2 min, glm::vec2 max);
    void ClearClip();
    void Draw(const Sprite& sprite);
    void DrawRect(glm::vec2 topLeft, glm::vec2 size, Color color, int32_t layer = 0);
    // A solid quadrilateral by its corners (a trapezoid, for something seen in perspective). The
    // two triangles meet on the diagonal from top right to bottom left, so the shape should be
    // convex.
    void DrawQuad(glm::vec2 topLeft, glm::vec2 topRight, glm::vec2 bottomLeft, glm::vec2 bottomRight, Color color,
                  int32_t layer = 0);
    // A solid triangle.
    void DrawTriangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color color, int32_t layer = 0);
    // A triangle showing the part of `texture` under it, the texture laid out over imageMin to
    // imageMax (virtual units), as through a window: artwork cut to a shape, unstretched. Tinted
    // by `tint`.
    void DrawTriangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, rhi::TextureHandle texture, glm::vec2 imageMin,
                      glm::vec2 imageMax, Color tint, int32_t layer = 0);
    // A triangle lit at its edges: `color` there, fading inward over `width` (virtual units) to
    // `inner` of it at the middle (0 for a glowing outline, more for a pane of glass), its outer
    // edge smoothed. Found per pixel from the distances to its sides.
    void DrawTriangleGlow(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color color, float width, float inner = 0.0f,
                          int32_t layer = 0);
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
    // Matches SpriteInstance in shaders/Sprite.slang: the corners, top left and top right, then
    // bottom left and bottom right.
    struct Instance
    {
        glm::vec4 TopCorners;
        glm::vec4 BottomCorners;
        glm::vec4 UvRect;
        uint32_t Color;
        // Bit 0: IsDistanceField. Bit 1: UvRect is where the texture lies, in virtual units
        // (DrawTriangle's window). Bit 2: a glow at a triangle's edges, UvRect its width and what's
        // left at the middle (DrawTriangleGlow).
        uint32_t Flags;
        // Min x, min y, max x, max y, in virtual units.
        glm::vec4 ClipRect;
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
    float opacity = 1.0f;
    // Sprites outside a clip of this size don't exist.
    static constexpr float NO_CLIP = 1.0e9f;
    glm::vec4 clip{-NO_CLIP, -NO_CLIP, NO_CLIP, NO_CLIP};
    std::vector<Entry> entries;
    std::vector<int32_t> orderedLayers;
    std::vector<Instance> instances;
    uint32_t lastSpriteCount = 0;
    uint32_t lastDrawCallCount = 0;
};

} // namespace hyoshi::renderer
