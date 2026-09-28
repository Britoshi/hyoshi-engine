#pragma once

#include "core/Handle.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hyoshi::rhi
{

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct PipelineTag;
using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using PipelineHandle = Handle<PipelineTag>;

// Frames the CPU may record ahead of the GPU. Data rewritten every frame (CpuToGpu buffers) needs
// this many copies, indexed by IRenderDevice::GetFrameIndex().
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

enum class FrameStatus
{
    // A swapchain image was acquired and the command list is recording.
    Ready,
    // Nothing to render into right now: no surface, a zero-sized window, or the swapchain is being
    // recreated. Skip the frame.
    Skipped
};

enum class SurfaceTransform
{
    Identity,
    Rotate90,
    Rotate180,
    Rotate270
};

struct Extent
{
    uint32_t Width = 0;
    uint32_t Height = 0;
};

struct ClearColor
{
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    float A = 1.0f;
};

// For now every render pass targets the swapchain, clears it, and stores the result.
struct RenderPassDesc
{
    ClearColor Clear;
};

enum class BufferUsage
{
    Vertex,
    Index
};

enum class MemoryUsage
{
    // Device-local; filled with UploadBuffer, typically once.
    GpuOnly,
    // Host-visible and persistently mapped; rewritten every frame (keep one per frame in flight).
    CpuToGpu
};

struct BufferDesc
{
    size_t Size = 0;
    BufferUsage Usage = BufferUsage::Vertex;
    MemoryUsage Memory = MemoryUsage::GpuOnly;
    const char* DebugName = "buffer";
};

enum class TextureFormat
{
    Rgba8Unorm,
    R8Unorm
};

// Sampled 2D textures. Contents are undefined until UpdateTexture.
struct TextureDesc
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    TextureFormat Format = TextureFormat::Rgba8Unorm;
    const char* DebugName = "texture";
};

enum class Filter
{
    Nearest,
    Linear
};

enum class AddressMode
{
    ClampToEdge,
    Repeat
};

struct SamplerDesc
{
    Filter MinMagFilter = Filter::Linear;
    AddressMode Address = AddressMode::ClampToEdge;
};

enum class IndexType
{
    UInt16,
    UInt32
};

enum class VertexFormat
{
    Float,
    Float2,
    Float3,
    Float4,
    // Four bytes normalized to 0..1, e.g. an RGBA8 color.
    UByte4Norm,
    UInt
};

enum class VertexInputRate
{
    Vertex,
    Instance
};

struct VertexBindingDesc
{
    uint32_t Binding = 0;
    uint32_t Stride = 0;
    VertexInputRate InputRate = VertexInputRate::Vertex;
};

struct VertexAttributeDesc
{
    uint32_t Location = 0;
    uint32_t Binding = 0;
    VertexFormat Format = VertexFormat::Float;
    uint32_t Offset = 0;
};

enum class PrimitiveTopology
{
    TriangleList,
    TriangleStrip
};

enum class BlendMode
{
    Opaque,
    // Straight (non-premultiplied) alpha.
    Alpha
};

struct PipelineDesc
{
    // One SPIR-V module holding both entry points, as produced by hyoshi_add_shaders().
    std::span<const uint32_t> Spirv;
    const char* VertexEntry = "vertexMain";
    const char* FragmentEntry = "fragmentMain";

    std::vector<VertexBindingDesc> VertexBindings;
    std::vector<VertexAttributeDesc> VertexAttributes;

    PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
    BlendMode Blend = BlendMode::Opaque;

    // Bytes of push constants visible to both stages. 128 is the minimum every device supports.
    uint32_t PushConstantSize = 0;

    // Textures sampled by the fragment stage, at set 0, bindings 0..TextureCount-1. In Slang:
    // [[vk::binding(0, 0)]] Sampler2D texture;
    uint32_t TextureCount = 0;

    const char* DebugName = "pipeline";
};

struct Viewport
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
};

struct Rect
{
    int32_t X = 0;
    int32_t Y = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
};

struct DeviceConfig
{
    std::string ApplicationName = "Hyoshi";

    // Enables the Khronos validation layer and routes its messages to the log.
    bool EnableValidation = false;

    // Where the pipeline cache is loaded from and saved to. Empty disables persistence.
    std::string PipelineCachePath;
};

} // namespace hyoshi::rhi
