#pragma once

#include "core/Handle.h"
#include "core/Result.h"
#include "renderer/Camera3D.h"
#include "renderer/Mesh.h"
#include "renderer/SpriteBatch.h"
#include "rhi/IRenderDevice.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hyoshi::renderer
{

struct MeshTag;
using MeshHandle = Handle<MeshTag>;

enum class MaterialBlend : uint8_t
{
    // Solid: hides what's behind it.
    Opaque,
    // Seen through by its alpha, drawn after the solid, farthest first.
    Alpha,
    // Its color added to what's behind: light, flames, magic. Drawn last.
    Additive
};

struct Material
{
    // Multiplies the texture and the mesh's vertex colors.
    Color BaseColor = WHITE;
    // None: white.
    rhi::TextureHandle Texture;
    // 0, lit by the scene's light; 1, its own color whatever the light (unlit); between, both.
    float Emission = 0.0f;
    // Light along its silhouette, as if from behind it.
    float Rim = 0.0f;
    // 0, shaded smoothly; 1, as a cartoon is (toon, cel): the light's side and the shadow's split by
    // a soft edge, the shadow tinted, the rim a band; between, both. In steps of 1/255.
    float Toon = 0.0f;
    MaterialBlend Blend = MaterialBlend::Opaque;
    // Both faces drawn and lit, for thin things (a page, a cloth); otherwise backs aren't drawn.
    bool IsTwoSided = false;
    // Which of the scene's lightings lights it (Renderer3D::SetLighting): a room by candlelight and
    // the night outside its window, say.
    uint8_t LightingSlot = 0;
};

// A light: one, from a direction, with an ambient that runs from the ground's color (on
// what faces down) to the sky's (on what faces up), and fog that fades distant things into its color.
struct Lighting
{
    // Toward the light, in world space.
    glm::vec3 Direction{0.4f, 1.0f, 0.6f};
    Color Light{255, 244, 228, 255};
    Color Sky{70, 76, 100, 255};
    Color Ground{30, 26, 24, 255};
    Color Fog{10, 10, 18, 255};
    // How fast fog thickens with distance: the share of light lost is 1 - e^(-density x distance).
    // From 0 to 1 per unit, in steps of 1/255.
    float FogDensity = 0.0f;
};

// Draws meshes in 3D (decision 0003), before the 2D, which draws over them. A scene makes its meshes
// once, sets the camera and the light, and draws each mesh every frame it should be seen; the app
// renders what was drawn and forgets it. Only where the device has depth (IsAvailable).
class Renderer3D
{
public:
    Result<void> Initialize(rhi::IRenderDevice& device);
    void Shutdown();

    // False where the device has no depth buffer (the web, for now): nothing is drawn, and meshes
    // can't be made.
    bool IsAvailable() const
    {
        return isAvailable;
    }

    Result<MeshHandle> CreateMesh(const MeshData& data, const char* debugName = "mesh");
    void Destroy(MeshHandle mesh);

    void SetCamera(const Camera3D& newCamera)
    {
        camera = newCamera;
    }
    const Camera3D& GetCamera() const
    {
        return camera;
    }
    // A scene has up to LIGHTING_SLOTS lightings; each material takes one (Material::LightingSlot).
    static constexpr size_t LIGHTING_SLOTS = 4;
    void SetLighting(const Lighting& newLighting, size_t slot = 0)
    {
        if (slot < LIGHTING_SLOTS)
        {
            lightings[slot] = newLighting;
        }
    }

    // Draws the mesh this frame, placed by `transform` (moves, turns, and sizes; sizes alike on each
    // axis keep the lighting right).
    void Draw(MeshHandle mesh, const glm::mat4& transform, const Material& material);

    // The app's: draws this frame's meshes inside the render pass, then forgets them.
    void Render(rhi::ICommandList& commands, rhi::Extent swapchainExtent, rhi::SurfaceTransform surfaceTransform);

private:
    struct GpuMesh
    {
        rhi::BufferHandle Vertices;
        rhi::BufferHandle Indices;
        uint32_t IndexCount = 0;
    };

    struct DrawItem
    {
        MeshHandle Mesh;
        glm::mat4 Transform{1.0f};
        Material Surface;
        // Ordering: solid, then see-through, then added; within each, by distance from the camera.
        float SortKey = 0.0f;
    };

    // Opaque one-sided, opaque two-sided, alpha, additive.
    static constexpr size_t PIPELINE_COUNT = 4;

    static size_t GetPipelineIndex(const Material& material);

    rhi::IRenderDevice* device = nullptr;
    bool isAvailable = false;
    std::array<rhi::PipelineHandle, PIPELINE_COUNT> pipelines{};
    rhi::SamplerHandle sampler;
    rhi::TextureHandle whiteTexture;
    HandlePool<GpuMesh, MeshTag> meshes;
    Camera3D camera;
    std::array<Lighting, LIGHTING_SLOTS> lightings{};
    std::vector<DrawItem> draws;
};

} // namespace hyoshi::renderer
