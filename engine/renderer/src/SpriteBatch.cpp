#include "renderer/SpriteBatch.h"

#include "Sprite.spv.h"

#include "core/Log.h"

#include <algorithm>
#include <cstddef>

namespace hyoshi::renderer
{

namespace
{

constexpr size_t INITIAL_CAPACITY = 1024;

static_assert(sizeof(ClipTransform) == 24, "ClipTransform must match DrawConstants in the 2D shaders");

uint32_t PackColor(Color color)
{
    // Byte order R, G, B, A in memory, read by the shader as UByte4Norm.
    return uint32_t{color.R} | (uint32_t{color.G} << 8) | (uint32_t{color.B} << 16) | (uint32_t{color.A} << 24);
}

uint64_t SortKey(rhi::TextureHandle texture)
{
    return (uint64_t{texture.Generation} << 32) | texture.Index;
}

} // namespace

Result<void> SpriteBatch::Initialize(rhi::IRenderDevice& renderDevice)
{
    device = &renderDevice;

    rhi::PipelineDesc pipelineDesc;
    pipelineDesc.Spirv = SPRITE_SPIRV;
    pipelineDesc.Topology = rhi::PrimitiveTopology::TriangleStrip;
    pipelineDesc.Blend = rhi::BlendMode::Alpha;
    pipelineDesc.PushConstantSize = sizeof(ClipTransform);
    pipelineDesc.TextureCount = 1;
    pipelineDesc.VertexBindings = {{0, sizeof(Instance), rhi::VertexInputRate::Instance}};
    pipelineDesc.VertexAttributes = {
        {0, 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(Instance, Center))},
        {1, 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(Instance, Size))},
        {2, 0, rhi::VertexFormat::Float, static_cast<uint32_t>(offsetof(Instance, Angle))},
        {3, 0, rhi::VertexFormat::Float4, static_cast<uint32_t>(offsetof(Instance, UvRect))},
        {4, 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(Instance, Color))},
        {5, 0, rhi::VertexFormat::UInt, static_cast<uint32_t>(offsetof(Instance, Flags))},
    };
    pipelineDesc.DebugName = "Sprite";

    Result<rhi::PipelineHandle> createdPipeline = device->CreatePipeline(pipelineDesc);
    if (!createdPipeline)
    {
        return createdPipeline.GetError();
    }
    pipeline = createdPipeline.Value();

    Result<rhi::SamplerHandle> createdSampler = device->CreateSampler({});
    if (!createdSampler)
    {
        return createdSampler.GetError();
    }
    sampler = createdSampler.Value();

    // Solid-color sprites sample a 1x1 white texture, so every sprite goes through one pipeline.
    rhi::TextureDesc whiteDesc;
    whiteDesc.Width = 1;
    whiteDesc.Height = 1;
    whiteDesc.DebugName = "SpriteBatch white";
    Result<rhi::TextureHandle> createdTexture = device->CreateTexture(whiteDesc);
    if (!createdTexture)
    {
        return createdTexture.GetError();
    }
    whiteTexture = createdTexture.Value();

    const uint32_t whitePixel = 0xFFFFFFFFu;
    return device->UpdateTexture(whiteTexture, &whitePixel, 0, 0, 1, 1);
}

void SpriteBatch::Shutdown()
{
    if (device == nullptr)
    {
        return;
    }

    for (rhi::BufferHandle& buffer : instanceBuffers)
    {
        device->Destroy(buffer);
        buffer = {};
    }
    instanceCapacities = {};
    device->Destroy(whiteTexture);
    device->Destroy(sampler);
    device->Destroy(pipeline);
    device = nullptr;
}

void SpriteBatch::Begin(const Camera2D& camera)
{
    clipTransform = camera.GetClipTransform();
    entries.clear();
    orderedLayers.clear();
}

void SpriteBatch::KeepOrder(int32_t layer)
{
    if (std::find(orderedLayers.begin(), orderedLayers.end(), layer) == orderedLayers.end())
    {
        orderedLayers.push_back(layer);
    }
}

void SpriteBatch::Draw(const Sprite& sprite)
{
    Entry entry;
    entry.Layer = sprite.Layer;
    entry.Texture = sprite.Texture.IsValid() ? sprite.Texture : whiteTexture;
    entry.TextureKey = 0;
    entry.Data = {sprite.Center,          sprite.Size,
                  sprite.Rotation,        sprite.UvRect,
                  PackColor(sprite.Tint), sprite.IsDistanceField ? 1u : 0u};
    entries.push_back(entry);
}

void SpriteBatch::DrawRect(glm::vec2 topLeft, glm::vec2 size, Color color, int32_t layer)
{
    Sprite sprite;
    sprite.Center = topLeft + size * 0.5f;
    sprite.Size = size;
    sprite.Tint = color;
    sprite.Layer = layer;
    Draw(sprite);
}

void SpriteBatch::End(rhi::ICommandList& commands)
{
    lastSpriteCount = static_cast<uint32_t>(entries.size());
    lastDrawCallCount = 0;
    if (entries.empty())
    {
        return;
    }

    for (Entry& entry : entries)
    {
        const bool isOrdered =
            std::find(orderedLayers.begin(), orderedLayers.end(), entry.Layer) != orderedLayers.end();
        entry.TextureKey = isOrdered ? 0 : SortKey(entry.Texture);
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b)
                     {
                         if (a.Layer != b.Layer)
                         {
                             return a.Layer < b.Layer;
                         }
                         return a.TextureKey < b.TextureKey;
                     });

    instances.resize(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        instances[i] = entries[i].Data;
    }

    const uint32_t frame = device->GetFrameIndex();
    if (Result<void> result = EnsureCapacity(frame, instances.size()); !result)
    {
        HYOSHI_LOG_ERROR("SpriteBatch: {}", result.GetError().Message);
        return;
    }
    if (Result<void> result =
            device->UploadBuffer(instanceBuffers[frame], instances.data(), instances.size() * sizeof(Instance), 0);
        !result)
    {
        HYOSHI_LOG_ERROR("SpriteBatch: {}", result.GetError().Message);
        return;
    }

    commands.BindPipeline(pipeline);
    commands.PushConstants(&clipTransform, sizeof(clipTransform));
    commands.BindVertexBuffer(0, instanceBuffers[frame], 0);

    // One instanced draw per run of consecutive sprites with the same texture.
    size_t runStart = 0;
    while (runStart < entries.size())
    {
        size_t runEnd = runStart + 1;
        while (runEnd < entries.size() && entries[runEnd].Texture == entries[runStart].Texture)
        {
            ++runEnd;
        }

        commands.BindTexture(0, entries[runStart].Texture, sampler);
        commands.Draw(4, static_cast<uint32_t>(runEnd - runStart), 0, static_cast<uint32_t>(runStart));
        ++lastDrawCallCount;
        runStart = runEnd;
    }
}

Result<void> SpriteBatch::EnsureCapacity(uint32_t frame, size_t spriteCount)
{
    if (spriteCount <= instanceCapacities[frame])
    {
        return {};
    }

    size_t capacity = std::max(instanceCapacities[frame], INITIAL_CAPACITY);
    while (capacity < spriteCount)
    {
        capacity *= 2;
    }

    device->Destroy(instanceBuffers[frame]);
    instanceBuffers[frame] = {};
    instanceCapacities[frame] = 0;

    rhi::BufferDesc bufferDesc;
    bufferDesc.Size = capacity * sizeof(Instance);
    bufferDesc.Usage = rhi::BufferUsage::Vertex;
    bufferDesc.Memory = rhi::MemoryUsage::CpuToGpu;
    bufferDesc.DebugName = "SpriteBatch instances";
    Result<rhi::BufferHandle> buffer = device->CreateBuffer(bufferDesc);
    if (!buffer)
    {
        return buffer.GetError();
    }

    instanceBuffers[frame] = buffer.Value();
    instanceCapacities[frame] = capacity;
    return {};
}

} // namespace hyoshi::renderer
