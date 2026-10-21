#pragma once

#include "core/Handle.h"
#include "core/Result.h"
#include "renderer/Camera3D.h"
#include "renderer/Mesh.h"
#include "renderer/Model.h"
#include "renderer/SpriteBatch.h"
#include "rhi/IRenderDevice.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace hyoshi::renderer
{

struct MeshTag;
using MeshHandle = Handle<MeshTag>;

// Which of this frame's joints (Renderer3D::AddJoints) a skinned mesh is drawn with; none: as it
// rests.
struct JointSet
{
    static constexpr uint32_t NONE = UINT32_MAX;
    uint32_t Index = NONE;

    bool IsValid() const
    {
        return Index != NONE;
    }
};

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
    // Light baked into textures (a light bake, ray traced offline), two groups of it its lighting
    // dims or flickers apart (Lighting::LightmapWeights), at the mesh's MeshVertex::LightUv: each
    // RGBA8, the light's square root in its color (so the dark keeps its steps), its alpha where it's
    // baked. Where they're baked they light it instead of the lighting's light, ambient, and lights at
    // points (only its rim stays); none, or where they aren't, it's lit as it would be. Not on the web.
    std::array<rhi::TextureHandle, 2> Lightmaps{};
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
    // A grade on what it lights, last: brighter (Exposure, times), and richer or greyer (Saturation,
    // 1 as it is, 0 grey). Graded, colours past white roll off instead of clipping.
    float Exposure = 1.0f;
    float Saturation = 1.0f;
    // How bright each of the lightmaps lights (Material::Lightmaps): 1 as it was baked (times its
    // light's range, if it was baked scaled down to fit), less to dim it, a little more or less each
    // frame to flicker it.
    std::array<float, 2> LightmapWeights{1.0f, 1.0f};
};

// A light at a point (a candle, a lamp, a fire) as well as the lighting's own: brightest at it,
// falling off as light does (with the square of the distance from a metre's softness, so world
// units are metres) and to nothing `Range` away, on whatever faces it (and a little on what
// doesn't, as light wraps round in a room). Each frame's (Renderer3D::AddPointLight).
struct PointLight
{
    glm::vec3 Position{0.0f};
    float Range = 2.0f;
    Color Light{255, 190, 120, 255};
    // 1 lights a white surface facing it, close by, as bright as its color (half that a metre away).
    float Intensity = 1.0f;
    // Which lightings it adds to (Material::LightingSlot), a bit each: a candle lights the room, not
    // the night outside.
    uint8_t Slots = 0xFF;
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
    // A mesh whose vertices change while it's drawn (a face's blend shapes, worked out on the CPU):
    // UpdateMesh gives it new vertices, as many and in the same order as it was made with (its
    // triangles stay), seen from the next frame drawn. Each frame in flight has its own copy.
    Result<MeshHandle> CreateDynamicMesh(const MeshData& data, const char* debugName = "mesh");
    Result<void> UpdateMesh(MeshHandle mesh, std::span<const MeshVertex> vertices);
    // A skinned mesh (a model's skinned part: `skin`, one for each vertex), moved by its joints on the
    // GPU as it's drawn (Draw, with AddJoints's), so the CPU sends joints each frame, not vertices.
    // `isDynamic`: its vertices at rest change too (blend shapes: UpdateMesh, as CreateDynamicMesh's).
    Result<MeshHandle> CreateSkinnedMesh(const MeshData& data, std::span<const VertexJoints> skin, bool isDynamic,
                                         const char* debugName = "mesh");
    // A skinned mesh's vertices bound to other joints (one for each, as it was made with): a cloth
    // bound to its points again, say. Seen from the next frame drawn.
    Result<void> UpdateSkin(MeshHandle mesh, std::span<const VertexJoints> skin);
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

    // Lights at points this frame, forgotten after it: each lighting is lit by the brightest
    // MAX_POINT_LIGHTS of those for it. None where there's no 3D (nor on the web).
    static constexpr size_t MAX_POINT_LIGHTS = 7;
    void AddPointLight(const PointLight& light);

    // Joints to draw skinned meshes with this frame (PoseJoints's: each joint's node transform times
    // its inverse bind), forgotten after it; none where there's no 3D. One set serves every part of
    // a model in a pose.
    JointSet AddJoints(std::span<const glm::mat4> joints);

    // Draws the mesh this frame, placed by `transform` (moves, turns, and sizes; sizes alike on each
    // axis keep the lighting right); a skinned mesh moved by `joints` (without, as it rests).
    void Draw(MeshHandle mesh, const glm::mat4& transform, const Material& material, JointSet joints = {});

    // What's been drawn so far this frame, and its lights at points and lightings: for a tool that
    // writes the scene out to be lit elsewhere (a light bake), not for drawing.
    struct Drawn
    {
        MeshHandle Mesh;
        glm::mat4 Transform{1.0f};
        Material Surface;
        bool IsSkinned = false;
    };
    std::vector<Drawn> GetDrawn() const;
    std::span<const PointLight> GetPointLights() const
    {
        return pointLights;
    }
    const Lighting& GetLighting(size_t slot) const
    {
        return lightings[std::min(slot, LIGHTING_SLOTS - 1)];
    }

    // The app's: draws this frame's meshes inside the render pass, then forgets them.
    void Render(rhi::ICommandList& commands, rhi::Extent swapchainExtent, rhi::SurfaceTransform surfaceTransform);

private:
    struct GpuMesh
    {
        rhi::BufferHandle Vertices;
        rhi::BufferHandle Indices;
        uint32_t IndexCount = 0;
        uint32_t VertexCount = 0;
        // A dynamic mesh's: a copy of its vertices for each frame in flight, its latest vertices,
        // and which frames' copies are behind them (a bit each).
        std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> FrameVertices{};
        std::vector<MeshVertex> Latest;
        uint32_t StaleFrames = 0;
        // A skinned mesh's: each vertex's joints and weights (the second vertex stream).
        rhi::BufferHandle Skin;
    };

    struct DrawItem
    {
        MeshHandle Mesh;
        glm::mat4 Transform{1.0f};
        Material Surface;
        // Ordering: solid, then see-through, then added; within each, by distance from the camera.
        float SortKey = 0.0f;
        JointSet Joints;
    };

    // Opaque one-sided, opaque two-sided, alpha, additive.
    static constexpr size_t PIPELINE_COUNT = 4;

    static size_t GetPipelineIndex(const Material& material);
    // Each vertex's joints and weights, packed into a buffer on the GPU (a skinned mesh's second
    // vertex stream).
    Result<rhi::BufferHandle> CreateSkinBuffer(std::span<const VertexJoints> skin, const char* debugName);

    rhi::IRenderDevice* device = nullptr;
    bool isAvailable = false;
    std::array<rhi::PipelineHandle, PIPELINE_COUNT> pipelines{};
    // The same, for skinned meshes.
    std::array<rhi::PipelineHandle, PIPELINE_COUNT> skinnedPipelines{};
    rhi::SamplerHandle sampler;
    // A lightmap's: smooth between its texels, kept to its edges (its pieces lie side by side).
    rhi::SamplerHandle lightmapSampler;
    rhi::TextureHandle whiteTexture;
    // A material's lightmaps where it has none: baked nowhere (its alpha 0).
    rhi::TextureHandle unbakedTexture;
    HandlePool<GpuMesh, MeshTag> meshes;
    Camera3D camera;
    std::array<Lighting, LIGHTING_SLOTS> lightings{};
    std::vector<DrawItem> draws;
    // This frame's joints (AddJoints), three rows each, each set from a row the storage buffer's
    // alignment allows; each set's first row and rows; and the buffer they go up in, for each frame in
    // flight, with its size.
    std::vector<glm::vec4> jointRows;
    std::vector<std::pair<size_t, size_t>> jointSets;
    std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> jointBuffers{};
    std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT> jointBufferSizes{};
    // This frame's lights at points (AddPointLight); each draw's (in its object's space, with its
    // lighting's grade: Mesh.slang's pointLights), a block each, shared by draws placed alike; and
    // the buffer they go up in, for each frame in flight, with its size.
    std::vector<PointLight> pointLights;
    std::vector<glm::vec4> lightRows;
    std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> lightBuffers{};
    std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT> lightBufferSizes{};
};

} // namespace hyoshi::renderer
