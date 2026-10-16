#include "renderer/Renderer3D.h"

#if defined(__EMSCRIPTEN__)
#include "Mesh.wgsl.h"
#else
#include "Mesh.spv.h"
#endif

#include "core/Log.h"

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hyoshi::renderer
{
namespace
{

// Each draw's push constants (shaders/Mesh.slang's MeshConstants): 128 bytes, the most every device
// is sure to have.
struct MeshConstants
{
    // Object space to clip space, by columns.
    std::array<glm::vec4, 4> ClipColumns{};
    // Toward the light, in object space; w: emission.
    glm::vec4 LightDirection{0.0f};
    // The camera, in object space; w: the rim's strength.
    glm::vec4 CameraPosition{0.0f};
    // The world's up in object space, then the base color's bits (RGBA8), which the shader reads as
    // an integer: kept an integer here, so no float conversion can touch them.
    glm::vec3 Up{0.0f, 1.0f, 0.0f};
    uint32_t BaseColor = 0xFFFFFFFF;
    // The light, the sky, the ground, and the fog (its alpha the density), RGBA8 each.
    std::array<uint32_t, 4> Colors{};
};
static_assert(sizeof(MeshConstants) == 128, "Mesh push constants must fit in 128 bytes");

// The fog's alpha that gives a density of 1 per unit (Mesh.slang's FOG_DENSITY_SCALE).
constexpr float FOG_ALPHA_PER_DENSITY = 255.0f;

uint32_t PackFog(Color fog, float density)
{
    fog.A = static_cast<uint8_t>(std::lround(std::clamp(density * FOG_ALPHA_PER_DENSITY, 0.0f, 255.0f)));
    return PackMeshColor(fog);
}

// Solid, then see-through, then added.
int32_t GetPass(MaterialBlend blend)
{
    switch (blend)
    {
    case MaterialBlend::Alpha:
        return 1;
    case MaterialBlend::Additive:
        return 2;
    case MaterialBlend::Opaque:
        break;
    }
    return 0;
}

} // namespace

Result<void> Renderer3D::Initialize(rhi::IRenderDevice& renderDevice)
{
    device = &renderDevice;
    if (!device->HasDepth())
    {
        HYOSHI_LOG_INFO("3D: no depth buffer on this device, so no 3D");
        return {};
    }

#if !defined(__EMSCRIPTEN__)
    for (size_t i = 0; i < PIPELINE_COUNT; ++i)
    {
        rhi::PipelineDesc desc;
        desc.Spirv = MESH_SPIRV;
        desc.PushConstantSize = sizeof(MeshConstants);
        desc.TextureCount = 1;
        desc.VertexBindings = {{0, sizeof(MeshVertex), rhi::VertexInputRate::Vertex}};
        desc.VertexAttributes = {
            {0, 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(MeshVertex, Position))},
            {1, 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(MeshVertex, Normal))},
            {2, 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(MeshVertex, Uv))},
            {3, 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(MeshVertex, Color))},
        };
        // Triangles wound counter-clockwise seen from the front stay so on screen (Camera3D).
        desc.Front = rhi::FrontFace::CounterClockwise;
        switch (i)
        {
        case 0:
            desc.Depth = rhi::DepthMode::TestAndWrite;
            desc.Cull = rhi::CullMode::Back;
            desc.DebugName = "Mesh (opaque)";
            break;
        case 1:
            desc.Depth = rhi::DepthMode::TestAndWrite;
            desc.DebugName = "Mesh (opaque, two-sided)";
            break;
        case 2:
            // Seen through: tested against the solid, but hiding nothing.
            desc.Depth = rhi::DepthMode::Test;
            desc.Blend = rhi::BlendMode::Alpha;
            desc.DebugName = "Mesh (alpha)";
            break;
        default:
            desc.Depth = rhi::DepthMode::Test;
            desc.Blend = rhi::BlendMode::Additive;
            desc.DebugName = "Mesh (additive)";
            break;
        }
        Result<rhi::PipelineHandle> pipeline = device->CreatePipeline(desc);
        if (!pipeline)
        {
            return pipeline.GetError();
        }
        pipelines[i] = pipeline.Value();
    }
#endif

    rhi::SamplerDesc samplerDesc;
    samplerDesc.MinMagFilter = rhi::Filter::Linear;
    samplerDesc.Address = rhi::AddressMode::Repeat;
    Result<rhi::SamplerHandle> createdSampler = device->CreateSampler(samplerDesc);
    if (!createdSampler)
    {
        return createdSampler.GetError();
    }
    sampler = createdSampler.Value();

    rhi::TextureDesc whiteDesc;
    whiteDesc.Width = 1;
    whiteDesc.Height = 1;
    whiteDesc.DebugName = "3D white";
    Result<rhi::TextureHandle> white = device->CreateTexture(whiteDesc);
    if (!white)
    {
        return white.GetError();
    }
    whiteTexture = white.Value();
    constexpr uint32_t WHITE_PIXEL = 0xFFFFFFFF;
    if (Result<void> result = device->UpdateTexture(whiteTexture, &WHITE_PIXEL, 0, 0, 1, 1); !result)
    {
        return result;
    }

    isAvailable = true;
    return {};
}

void Renderer3D::Shutdown()
{
    if (device == nullptr)
    {
        return;
    }
    meshes.ForEach(
        [this](GpuMesh& mesh)
        {
            device->Destroy(mesh.Vertices);
            device->Destroy(mesh.Indices);
        });
    meshes.Clear();
    for (rhi::PipelineHandle& pipeline : pipelines)
    {
        if (pipeline.IsValid())
        {
            device->Destroy(pipeline);
            pipeline = {};
        }
    }
    if (sampler.IsValid())
    {
        device->Destroy(sampler);
        sampler = {};
    }
    if (whiteTexture.IsValid())
    {
        device->Destroy(whiteTexture);
        whiteTexture = {};
    }
    draws.clear();
    isAvailable = false;
    device = nullptr;
}

Result<MeshHandle> Renderer3D::CreateMesh(const MeshData& data, const char* debugName)
{
    if (!isAvailable)
    {
        return Error{"No 3D on this device"};
    }
    if (data.Vertices.empty() || data.Indices.empty())
    {
        return Error{std::string("An empty mesh (") + debugName + ")"};
    }
    GpuMesh mesh;
    mesh.IndexCount = static_cast<uint32_t>(data.Indices.size());
    const size_t vertexBytes = data.Vertices.size() * sizeof(MeshVertex);
    const size_t indexBytes = data.Indices.size() * sizeof(uint32_t);

    rhi::BufferDesc vertexDesc;
    vertexDesc.Size = vertexBytes;
    vertexDesc.Usage = rhi::BufferUsage::Vertex;
    vertexDesc.DebugName = debugName;
    Result<rhi::BufferHandle> vertices = device->CreateBuffer(vertexDesc);
    if (!vertices)
    {
        return vertices.GetError();
    }
    mesh.Vertices = vertices.Value();

    rhi::BufferDesc indexDesc;
    indexDesc.Size = indexBytes;
    indexDesc.Usage = rhi::BufferUsage::Index;
    indexDesc.DebugName = debugName;
    Result<rhi::BufferHandle> indices = device->CreateBuffer(indexDesc);
    Result<void> uploaded = indices ? device->UploadBuffer(mesh.Vertices, data.Vertices.data(), vertexBytes, 0)
                                    : Result<void>(indices.GetError());
    if (uploaded)
    {
        mesh.Indices = indices.Value();
        uploaded = device->UploadBuffer(mesh.Indices, data.Indices.data(), indexBytes, 0);
    }
    if (!uploaded)
    {
        device->Destroy(mesh.Vertices);
        if (mesh.Indices.IsValid())
        {
            device->Destroy(mesh.Indices);
        }
        return uploaded.GetError();
    }
    return meshes.Create(mesh);
}

void Renderer3D::Destroy(MeshHandle mesh)
{
    if (const GpuMesh* found = meshes.Get(mesh); found != nullptr)
    {
        // The device holds deletions until the frames using them are done.
        device->Destroy(found->Vertices);
        device->Destroy(found->Indices);
        meshes.Destroy(mesh);
    }
}

void Renderer3D::Draw(MeshHandle mesh, const glm::mat4& transform, const Material& material)
{
    if (isAvailable && meshes.Contains(mesh))
    {
        draws.push_back({mesh, transform, material, 0.0f});
    }
}

size_t Renderer3D::GetPipelineIndex(const Material& material)
{
    switch (material.Blend)
    {
    case MaterialBlend::Alpha:
        return 2;
    case MaterialBlend::Additive:
        return 3;
    case MaterialBlend::Opaque:
        break;
    }
    return material.IsTwoSided ? 1 : 0;
}

void Renderer3D::Render(rhi::ICommandList& commands, rhi::Extent swapchainExtent,
                        rhi::SurfaceTransform surfaceTransform)
{
    if (!isAvailable || draws.empty())
    {
        draws.clear();
        return;
    }

    // Solid nearest first (what's hidden then skips shading), the rest farthest first.
    for (DrawItem& item : draws)
    {
        item.SortKey = glm::distance(camera.Position, glm::vec3(item.Transform[3]));
    }
    std::ranges::stable_sort(draws,
                             [](const DrawItem& a, const DrawItem& b)
                             {
                                 const int32_t passA = GetPass(a.Surface.Blend);
                                 const int32_t passB = GetPass(b.Surface.Blend);
                                 if (passA != passB)
                                 {
                                     return passA < passB;
                                 }
                                 return passA == 0 ? a.SortKey < b.SortKey : a.SortKey > b.SortKey;
                             });

    const glm::mat4 clipFromWorld = camera.GetClipFromWorld(swapchainExtent, surfaceTransform);
    std::array<glm::vec3, LIGHTING_SLOTS> toLights{};
    std::array<std::array<uint32_t, 4>, LIGHTING_SLOTS> colors{};
    for (size_t slot = 0; slot < LIGHTING_SLOTS; ++slot)
    {
        const Lighting& lighting = lightings[slot];
        toLights[slot] = glm::normalize(lighting.Direction);
        colors[slot] = {PackMeshColor(lighting.Light), PackMeshColor(lighting.Sky), PackMeshColor(lighting.Ground),
                        PackFog(lighting.Fog, lighting.FogDensity)};
    }
    size_t boundPipeline = PIPELINE_COUNT;
    for (const DrawItem& item : draws)
    {
        const GpuMesh* mesh = meshes.Get(item.Mesh);
        if (mesh == nullptr)
        {
            continue;
        }
        if (const size_t pipeline = GetPipelineIndex(item.Surface); pipeline != boundPipeline)
        {
            commands.BindPipeline(pipelines[pipeline]);
            boundPipeline = pipeline;
        }
        commands.BindTexture(0, item.Surface.Texture.IsValid() ? item.Surface.Texture : whiteTexture, sampler);
        commands.BindVertexBuffer(0, mesh->Vertices, 0);
        commands.BindIndexBuffer(mesh->Indices, 0, rhi::IndexType::UInt32);

        // The light and the camera moved into the object's space, where its normals are.
        const size_t slot = std::min<size_t>(item.Surface.LightingSlot, LIGHTING_SLOTS - 1);
        const glm::mat4 objectFromWorld = glm::inverse(item.Transform);
        const glm::mat3 directionToObject(objectFromWorld);
        const glm::mat4 clipFromObject = clipFromWorld * item.Transform;
        MeshConstants constants;
        for (int32_t column = 0; column < 4; ++column)
        {
            constants.ClipColumns[static_cast<size_t>(column)] = clipFromObject[column];
        }
        constants.LightDirection = glm::vec4(directionToObject * toLights[slot], item.Surface.Emission);
        constants.CameraPosition =
            glm::vec4(glm::vec3(objectFromWorld * glm::vec4(camera.Position, 1.0f)), item.Surface.Rim);
        constants.Up = directionToObject * glm::vec3(0.0f, 1.0f, 0.0f);
        constants.BaseColor = PackMeshColor(item.Surface.BaseColor);
        constants.Colors = colors[slot];
        commands.PushConstants(&constants, sizeof(constants));
        commands.DrawIndexed(mesh->IndexCount, 1, 0, 0, 0);
    }
    draws.clear();
}

} // namespace hyoshi::renderer
