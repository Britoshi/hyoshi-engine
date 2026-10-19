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
#include <string>

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

// A skinned vertex's joints and weights, the second vertex stream (Mesh.slang's SkinnedInput): the
// weights as bytes (the first in memory first), 12 bytes in all, as phones like vertices small.
struct SkinVertex
{
    std::array<uint16_t, 4> Joints{};
    uint32_t Weights = 0;
};
static_assert(sizeof(SkinVertex) == 12, "A skinned vertex's joints and weights are 12 bytes");

// Rows of joints (three a joint) a joint set starts on, so its range starts where a storage buffer's
// may; and the least the joints' buffer is made (it grows to fit).
constexpr size_t JOINT_ROW_ALIGNMENT = rhi::STORAGE_BUFFER_ALIGNMENT / sizeof(glm::vec4);
constexpr size_t LEAST_JOINT_BUFFER = 64 * 1024;

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
    constexpr std::array<std::array<const char*, PIPELINE_COUNT>, 2> NAMES{{
        {"Mesh (opaque)", "Mesh (opaque, two-sided)", "Mesh (alpha)", "Mesh (additive)"},
        {"Mesh (opaque, skinned)", "Mesh (opaque, two-sided, skinned)", "Mesh (alpha, skinned)",
         "Mesh (additive, skinned)"},
    }};
    for (const bool isSkinned : {false, true})
    {
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
            if (isSkinned)
            {
                // Each vertex's joints and weights from a second stream, the joints from a storage
                // buffer.
                desc.VertexEntry = "vertexSkinned";
                desc.StorageBufferCount = 1;
                desc.VertexBindings.push_back({1, sizeof(SkinVertex), rhi::VertexInputRate::Vertex});
                desc.VertexAttributes.push_back(
                    {4, 1, rhi::VertexFormat::UShort4, static_cast<uint32_t>(offsetof(SkinVertex, Joints))});
                desc.VertexAttributes.push_back(
                    {5, 1, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(SkinVertex, Weights))});
            }
            // Triangles wound counter-clockwise seen from the front stay so on screen (Camera3D).
            desc.Front = rhi::FrontFace::CounterClockwise;
            desc.DebugName = NAMES[isSkinned ? 1 : 0][i];
            switch (i)
            {
            case 0:
                desc.Depth = rhi::DepthMode::TestAndWrite;
                desc.Cull = rhi::CullMode::Back;
                break;
            case 1:
                desc.Depth = rhi::DepthMode::TestAndWrite;
                break;
            case 2:
                // Seen through: tested against the solid, but hiding nothing.
                desc.Depth = rhi::DepthMode::Test;
                desc.Blend = rhi::BlendMode::Alpha;
                break;
            default:
                desc.Depth = rhi::DepthMode::Test;
                desc.Blend = rhi::BlendMode::Additive;
                break;
            }
            Result<rhi::PipelineHandle> pipeline = device->CreatePipeline(desc);
            if (!pipeline)
            {
                return pipeline.GetError();
            }
            (isSkinned ? skinnedPipelines : pipelines)[i] = pipeline.Value();
        }
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
            if (mesh.Skin.IsValid())
            {
                device->Destroy(mesh.Skin);
            }
        });
    meshes.Clear();
    for (std::array<rhi::PipelineHandle, PIPELINE_COUNT>* set : {&pipelines, &skinnedPipelines})
    {
        for (rhi::PipelineHandle& pipeline : *set)
        {
            if (pipeline.IsValid())
            {
                device->Destroy(pipeline);
                pipeline = {};
            }
        }
    }
    for (size_t frame = 0; frame < jointBuffers.size(); ++frame)
    {
        if (jointBuffers[frame].IsValid())
        {
            device->Destroy(jointBuffers[frame]);
        }
        jointBuffers[frame] = {};
        jointBufferSizes[frame] = 0;
    }
    jointRows.clear();
    jointSets.clear();
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
    mesh.VertexCount = static_cast<uint32_t>(data.Vertices.size());
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

Result<MeshHandle> Renderer3D::CreateDynamicMesh(const MeshData& data, const char* debugName)
{
    Result<MeshHandle> made = CreateMesh(data, debugName);
    if (!made)
    {
        return made;
    }
    GpuMesh* mesh = meshes.Get(made.Value());
    const size_t vertexBytes = data.Vertices.size() * sizeof(MeshVertex);
    for (rhi::BufferHandle& copy : mesh->FrameVertices)
    {
        rhi::BufferDesc desc;
        desc.Size = vertexBytes;
        desc.Usage = rhi::BufferUsage::Vertex;
        desc.Memory = rhi::MemoryUsage::CpuToGpu;
        desc.DebugName = debugName;
        Result<rhi::BufferHandle> buffer = device->CreateBuffer(desc);
        Result<void> uploaded = buffer ? device->UploadBuffer(buffer.Value(), data.Vertices.data(), vertexBytes, 0)
                                       : Result<void>(buffer.GetError());
        if (buffer)
        {
            copy = buffer.Value();
        }
        if (!uploaded)
        {
            Destroy(made.Value());
            return uploaded.GetError();
        }
    }
    mesh->Latest = data.Vertices;
    return made;
}

Result<MeshHandle> Renderer3D::CreateSkinnedMesh(const MeshData& data, std::span<const VertexJoints> skin,
                                                 bool isDynamic, const char* debugName)
{
    if (skin.size() != data.Vertices.size())
    {
        return Error{std::string("A skinned mesh needs joints for each vertex (") + debugName + ")"};
    }
    Result<MeshHandle> made = isDynamic ? CreateDynamicMesh(data, debugName) : CreateMesh(data, debugName);
    if (!made)
    {
        return made;
    }
    Result<rhi::BufferHandle> buffer = CreateSkinBuffer(skin, debugName);
    if (!buffer)
    {
        Destroy(made.Value());
        return buffer.GetError();
    }
    meshes.Get(made.Value())->Skin = buffer.Value();
    return made;
}

Result<void> Renderer3D::UpdateSkin(MeshHandle mesh, std::span<const VertexJoints> skin)
{
    GpuMesh* found = meshes.Get(mesh);
    if (found == nullptr || !found->Skin.IsValid())
    {
        return Error{"Not a skinned mesh"};
    }
    if (skin.size() != found->VertexCount)
    {
        return Error{"A skinned mesh keeps its number of vertices"};
    }
    Result<rhi::BufferHandle> buffer = CreateSkinBuffer(skin, "skin");
    if (!buffer)
    {
        return buffer.GetError();
    }
    // The device holds the old until the frames using it are done.
    device->Destroy(found->Skin);
    found->Skin = buffer.Value();
    return {};
}

Result<rhi::BufferHandle> Renderer3D::CreateSkinBuffer(std::span<const VertexJoints> skin, const char* debugName)
{
    std::vector<SkinVertex> packed(skin.size());
    for (size_t i = 0; i < skin.size(); ++i)
    {
        packed[i].Joints = skin[i].Joints;
        for (int k = 0; k < 4; ++k)
        {
            const auto weight = static_cast<uint32_t>(std::lround(std::clamp(skin[i].Weights[k], 0.0f, 1.0f) * 255.0f));
            packed[i].Weights |= weight << (8U * static_cast<uint32_t>(k));
        }
    }
    rhi::BufferDesc desc;
    desc.Size = packed.size() * sizeof(SkinVertex);
    desc.Usage = rhi::BufferUsage::Vertex;
    desc.DebugName = debugName;
    Result<rhi::BufferHandle> buffer = device->CreateBuffer(desc);
    if (!buffer)
    {
        return buffer;
    }
    if (Result<void> uploaded = device->UploadBuffer(buffer.Value(), packed.data(), desc.Size, 0); !uploaded)
    {
        device->Destroy(buffer.Value());
        return uploaded.GetError();
    }
    return buffer;
}

Result<void> Renderer3D::UpdateMesh(MeshHandle mesh, std::span<const MeshVertex> vertices)
{
    GpuMesh* found = meshes.Get(mesh);
    if (found == nullptr || !found->FrameVertices[0].IsValid())
    {
        return Error{"Not a dynamic mesh"};
    }
    if (vertices.size() != found->Latest.size())
    {
        return Error{"A dynamic mesh keeps its number of vertices"};
    }
    std::copy(vertices.begin(), vertices.end(), found->Latest.begin());
    found->StaleFrames = (1U << rhi::MAX_FRAMES_IN_FLIGHT) - 1U;
    return {};
}

void Renderer3D::Destroy(MeshHandle mesh)
{
    if (const GpuMesh* found = meshes.Get(mesh); found != nullptr)
    {
        // The device holds deletions until the frames using them are done.
        device->Destroy(found->Vertices);
        device->Destroy(found->Indices);
        for (const rhi::BufferHandle copy : found->FrameVertices)
        {
            if (copy.IsValid())
            {
                device->Destroy(copy);
            }
        }
        if (found->Skin.IsValid())
        {
            device->Destroy(found->Skin);
        }
        meshes.Destroy(mesh);
    }
}

JointSet Renderer3D::AddJoints(std::span<const glm::mat4> joints)
{
    if (!isAvailable || joints.empty())
    {
        return {};
    }
    // Each joint's matrix as three rows: Mesh.slang's jointRows.
    const size_t first = (jointRows.size() + JOINT_ROW_ALIGNMENT - 1) / JOINT_ROW_ALIGNMENT * JOINT_ROW_ALIGNMENT;
    jointRows.resize(first + (joints.size() * 3));
    for (size_t joint = 0; joint < joints.size(); ++joint)
    {
        const glm::mat4& matrix = joints[joint];
        for (int row = 0; row < 3; ++row)
        {
            jointRows[first + (joint * 3) + static_cast<size_t>(row)] =
                glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
        }
    }
    jointSets.emplace_back(first, joints.size() * 3);
    return {static_cast<uint32_t>(jointSets.size() - 1)};
}

void Renderer3D::Draw(MeshHandle mesh, const glm::mat4& transform, const Material& material, JointSet joints)
{
    if (isAvailable && meshes.Contains(mesh))
    {
        draws.push_back({mesh, transform, material, 0.0f, joints});
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
        jointRows.clear();
        jointSets.clear();
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
    const uint32_t frame = device->GetFrameIndex();
    // This frame's joints up to the GPU, in this frame's buffer (made bigger if it must be). Without
    // it, skinned meshes are drawn as they rest.
    rhi::BufferHandle joints;
    if (const size_t bytes = jointRows.size() * sizeof(glm::vec4); bytes > 0)
    {
        if (jointBufferSizes[frame] < bytes)
        {
            if (jointBuffers[frame].IsValid())
            {
                device->Destroy(jointBuffers[frame]);
            }
            rhi::BufferDesc desc;
            desc.Size = std::max(bytes * 2, LEAST_JOINT_BUFFER);
            desc.Usage = rhi::BufferUsage::Storage;
            desc.Memory = rhi::MemoryUsage::CpuToGpu;
            desc.DebugName = "3D joints";
            Result<rhi::BufferHandle> made = device->CreateBuffer(desc);
            jointBuffers[frame] = made ? made.Value() : rhi::BufferHandle{};
            jointBufferSizes[frame] = made ? desc.Size : 0;
            if (!made)
            {
                HYOSHI_LOG_ERROR("3D: joints: {}", made.GetError().Message);
            }
        }
        if (jointBuffers[frame].IsValid())
        {
            if (Result<void> uploaded = device->UploadBuffer(jointBuffers[frame], jointRows.data(), bytes, 0); uploaded)
            {
                joints = jointBuffers[frame];
            }
        }
    }
    size_t boundPipeline = PIPELINE_COUNT * 2;
    for (const DrawItem& item : draws)
    {
        GpuMesh* mesh = meshes.Get(item.Mesh);
        if (mesh == nullptr)
        {
            continue;
        }
        // A dynamic mesh draws this frame's copy, brought up to its latest vertices first.
        rhi::BufferHandle vertices = mesh->Vertices;
        if (mesh->FrameVertices[frame].IsValid())
        {
            vertices = mesh->FrameVertices[frame];
            if ((mesh->StaleFrames & (1U << frame)) != 0)
            {
                if (Result<void> uploaded = device->UploadBuffer(vertices, mesh->Latest.data(),
                                                                 mesh->Latest.size() * sizeof(MeshVertex), 0);
                    uploaded)
                {
                    mesh->StaleFrames &= ~(1U << frame);
                }
            }
        }
        const bool isSkinned =
            mesh->Skin.IsValid() && joints.IsValid() && item.Joints.IsValid() && item.Joints.Index < jointSets.size();
        const size_t pipeline = GetPipelineIndex(item.Surface);
        if (const size_t bound = pipeline + (isSkinned ? PIPELINE_COUNT : 0); bound != boundPipeline)
        {
            commands.BindPipeline((isSkinned ? skinnedPipelines : pipelines)[pipeline]);
            boundPipeline = bound;
        }
        commands.BindTexture(0, item.Surface.Texture.IsValid() ? item.Surface.Texture : whiteTexture, sampler);
        commands.BindVertexBuffer(0, vertices, 0);
        if (isSkinned)
        {
            const auto [firstRow, rows] = jointSets[item.Joints.Index];
            commands.BindVertexBuffer(1, mesh->Skin, 0);
            commands.BindStorageBuffer(0, joints, firstRow * sizeof(glm::vec4), rows * sizeof(glm::vec4));
        }
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
        // The light's alpha (unused by the light) carries how toon the surface is.
        const auto toon = static_cast<uint32_t>(std::lround(std::clamp(item.Surface.Toon, 0.0f, 1.0f) * 255.0f));
        constants.Colors[0] = (constants.Colors[0] & 0x00FFFFFFU) | (toon << 24U);
        commands.PushConstants(&constants, sizeof(constants));
        commands.DrawIndexed(mesh->IndexCount, 1, 0, 0, 0);
    }
    draws.clear();
    jointRows.clear();
    jointSets.clear();
}

} // namespace hyoshi::renderer
