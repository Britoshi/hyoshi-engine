#include "WebGpuRenderDevice.h"

#include "core/Log.h"
#include "platform/Platform.h"

#include <SDL3/SDL_video.h>
#include <emscripten/em_js.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

// The bodies are JavaScript, which clang-format would break.
// clang-format off

// The page counts uncaptured WebGPU errors (device.onuncapturederror) in Module.webgpuErrorCount.
EM_JS(int, GetWebGpuErrorCount, (), { return Module["webgpuErrorCount"] || 0; });

// 1 if the browser prefers BGRA8 for canvases, as on macOS; otherwise RGBA8.
EM_JS(int, PrefersBgraCanvas, (), { return navigator.gpu.getPreferredCanvasFormat() === "bgra8unorm" ? 1 : 0; });

// clang-format on

namespace hyoshi::rhi
{

namespace
{

WGPUStringView ToStringView(const char* text)
{
    return {text, WGPU_STRLEN};
}

WGPUVertexFormat ToWgpuFormat(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float:
        return WGPUVertexFormat_Float32;
    case VertexFormat::Float2:
        return WGPUVertexFormat_Float32x2;
    case VertexFormat::Float3:
        return WGPUVertexFormat_Float32x3;
    case VertexFormat::Float4:
        return WGPUVertexFormat_Float32x4;
    case VertexFormat::UByte4Norm:
        return WGPUVertexFormat_Unorm8x4;
    case VertexFormat::UInt:
        return WGPUVertexFormat_Uint32;
    }
    return WGPUVertexFormat_Float32;
}

uint64_t ToBits(uint32_t index, uint32_t generation)
{
    return (static_cast<uint64_t>(generation) << 32) | index;
}

// Queue writes take multiples of 4 bytes.
constexpr size_t AlignUp4(size_t size)
{
    return (size + 3) & ~size_t{3};
}

} // namespace

// --- WebGpuCommandList ---

void WebGpuCommandList::Reset(WGPUCommandEncoder newEncoder)
{
    encoder = newEncoder;
    pass = nullptr;
    boundTextureCount = 0;
    hasConstants = false;
    boundTextures = {};
    boundSamplers = {};
    areTexturesDirty = false;
    areConstantsDirty = false;
    constantsOffset = 0;
}

void WebGpuCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    HYOSHI_ASSERT(pass == nullptr, "Render passes can't nest");

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = device.currentView;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = {desc.Clear.R, desc.Clear.G, desc.Clear.B, desc.Clear.A};

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &color;
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);

    const Extent extent = device.surfaceExtent;
    SetViewport({0.0f, 0.0f, static_cast<float>(extent.Width), static_cast<float>(extent.Height)});
    SetScissor({0, 0, extent.Width, extent.Height});
}

void WebGpuCommandList::EndRenderPass()
{
    HYOSHI_ASSERT(pass != nullptr);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    pass = nullptr;
}

void WebGpuCommandList::EndOpenPass()
{
    if (pass != nullptr)
    {
        EndRenderPass();
    }
}

void WebGpuCommandList::BindPipeline(PipelineHandle pipeline)
{
    const WebGpuRenderDevice::PipelineData* data = device.pipelines.Get(pipeline);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed pipeline");
    wgpuRenderPassEncoderSetPipeline(pass, data->Pipeline);
    boundTextureCount = data->TextureCount;
    hasConstants = data->HasConstants;
    // A new pipeline needs its groups set again before the next draw.
    areTexturesDirty = true;
    areConstantsDirty = hasConstants;
}

void WebGpuCommandList::BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset)
{
    const WebGpuRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    wgpuRenderPassEncoderSetVertexBuffer(pass, binding, data->Buffer, offset, WGPU_WHOLE_SIZE);
}

void WebGpuCommandList::BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type)
{
    const WebGpuRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    wgpuRenderPassEncoderSetIndexBuffer(pass, data->Buffer,
                                        type == IndexType::UInt16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                        offset, WGPU_WHOLE_SIZE);
}

void WebGpuCommandList::BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler)
{
    HYOSHI_ASSERT(slot < MAX_TEXTURE_SLOTS);
    HYOSHI_ASSERT(device.textures.Contains(texture) && device.samplers.Contains(sampler),
                  "Invalid or destroyed texture or sampler");
    if (boundTextures[slot] == texture && boundSamplers[slot] == sampler)
    {
        return;
    }
    boundTextures[slot] = texture;
    boundSamplers[slot] = sampler;
    areTexturesDirty = true;
}

void WebGpuCommandList::PushConstants(const void* data, uint32_t size)
{
    HYOSHI_ASSERT(hasConstants, "Bind a pipeline with push constants before pushing them");
    constantsOffset = device.PushConstantData(data, size);
    areConstantsDirty = true;
}

void WebGpuCommandList::SetViewport(const Viewport& viewport)
{
    wgpuRenderPassEncoderSetViewport(pass, viewport.X, viewport.Y, viewport.Width, viewport.Height, 0.0f, 1.0f);
}

void WebGpuCommandList::SetScissor(const Rect& scissor)
{
    // WebGPU rejects rectangles outside the target, which Vulkan clips.
    const Extent extent = device.surfaceExtent;
    const auto x0 = static_cast<uint32_t>(std::clamp<int64_t>(scissor.X, 0, extent.Width));
    const auto y0 = static_cast<uint32_t>(std::clamp<int64_t>(scissor.Y, 0, extent.Height));
    const auto x1 =
        static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(scissor.X) + scissor.Width, 0, extent.Width));
    const auto y1 =
        static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(scissor.Y) + scissor.Height, 0, extent.Height));
    wgpuRenderPassEncoderSetScissorRect(pass, x0, y0, x1 - x0, y1 - y0);
}

void WebGpuCommandList::FlushBindings()
{
    if (areTexturesDirty && (boundTextureCount > 0 || hasConstants))
    {
        // Group 0 must be set whenever group 1 is used, even by a pipeline with no textures.
        const WGPUBindGroup group =
            device.GetTextureBindGroup(boundTextureCount, boundTextures.data(), boundSamplers.data());
        if (group != nullptr)
        {
            wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        }
    }
    areTexturesDirty = false;

    if (areConstantsDirty && hasConstants)
    {
        wgpuRenderPassEncoderSetBindGroup(pass, 1, device.constantBindGroup, 1, &constantsOffset);
    }
    areConstantsDirty = false;
}

void WebGpuCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    FlushBindings();
    wgpuRenderPassEncoderDraw(pass, vertexCount, instanceCount, firstVertex, firstInstance);
}

void WebGpuCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t firstInstance)
{
    FlushBindings();
    wgpuRenderPassEncoderDrawIndexed(pass, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// --- WebGpuRenderDevice: setup and teardown ---

WebGpuRenderDevice::WebGpuRenderDevice() : commandList(*this)
{
}

WebGpuRenderDevice::~WebGpuRenderDevice()
{
    Shutdown();
}

Result<void> WebGpuRenderDevice::Initialize(const DeviceConfig& /*config*/, platform::Platform& targetPlatform)
{
    HYOSHI_ASSERT(!isInitialized);
    platform = &targetPlatform;

    instance = wgpuCreateInstance(nullptr);
    if (instance == nullptr)
    {
        return Error{"wgpuCreateInstance failed"};
    }
    device = emscripten_webgpu_get_device();
    if (device == nullptr)
    {
        return Error{"No WebGPU device: the page must set Module.preinitializedWebGPUDevice"};
    }
    queue = wgpuDeviceGetQueue(device);
    surfaceFormat = PrefersBgraCanvas() != 0 ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;

    // The constant ring and its layout: one uniform buffer at group 1, offset per draw.
    WGPUBufferDescriptor bufferDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    bufferDesc.label = ToStringView("push constants");
    bufferDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bufferDesc.size = static_cast<uint64_t>(CONSTANT_SLOT_SIZE) * CONSTANT_SLOTS;
    constantBuffer = wgpuDeviceCreateBuffer(device, &bufferDesc);

    WGPUBindGroupLayoutEntry constantEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    constantEntry.binding = 0;
    constantEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    constantEntry.buffer.type = WGPUBufferBindingType_Uniform;
    constantEntry.buffer.hasDynamicOffset = true;
    WGPUBindGroupLayoutDescriptor layoutDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.label = ToStringView("push constants");
    layoutDesc.entryCount = 1;
    layoutDesc.entries = &constantEntry;
    constantLayout = wgpuDeviceCreateBindGroupLayout(device, &layoutDesc);

    WGPUBindGroupEntry constantBinding = WGPU_BIND_GROUP_ENTRY_INIT;
    constantBinding.binding = 0;
    constantBinding.buffer = constantBuffer;
    constantBinding.offset = 0;
    constantBinding.size = CONSTANT_SLOT_SIZE;
    WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    groupDesc.label = ToStringView("push constants");
    groupDesc.layout = constantLayout;
    groupDesc.entryCount = 1;
    groupDesc.entries = &constantBinding;
    constantBindGroup = wgpuDeviceCreateBindGroup(device, &groupDesc);

    if (Result<void> result = CreateSurface(); !result)
    {
        Shutdown();
        return result;
    }

    HYOSHI_LOG_INFO("WebGPU: canvas format {}", surfaceFormat == WGPUTextureFormat_BGRA8Unorm ? "BGRA8" : "RGBA8");
    isInitialized = true;
    return {};
}

void WebGpuRenderDevice::Shutdown()
{
    if (device == nullptr)
    {
        return;
    }

    for (auto& [hash, entries] : bindGroups)
    {
        for (BindGroupEntry& entry : entries)
        {
            wgpuBindGroupRelease(entry.BindGroup);
        }
    }
    bindGroups.clear();
    for (auto& [count, layout] : textureLayouts)
    {
        wgpuBindGroupLayoutRelease(layout);
    }
    textureLayouts.clear();

    pipelines.ForEach([](PipelineData& data) { wgpuRenderPipelineRelease(data.Pipeline); });
    pipelines.Clear();
    samplers.ForEach([](SamplerData& data) { wgpuSamplerRelease(data.Sampler); });
    samplers.Clear();
    textures.ForEach(
        [](TextureData& data)
        {
            wgpuTextureViewRelease(data.View);
            wgpuTextureRelease(data.Texture);
        });
    textures.Clear();
    buffers.ForEach([](BufferData& data) { wgpuBufferRelease(data.Buffer); });
    buffers.Clear();

    if (constantBindGroup != nullptr)
    {
        wgpuBindGroupRelease(constantBindGroup);
        constantBindGroup = nullptr;
    }
    if (constantLayout != nullptr)
    {
        wgpuBindGroupLayoutRelease(constantLayout);
        constantLayout = nullptr;
    }
    if (constantBuffer != nullptr)
    {
        wgpuBufferRelease(constantBuffer);
        constantBuffer = nullptr;
    }

    DestroySurface();
    wgpuQueueRelease(queue);
    queue = nullptr;
    wgpuDeviceRelease(device);
    device = nullptr;
    wgpuInstanceRelease(instance);
    instance = nullptr;
    isInitialized = false;
}

// --- Surface ---

Result<void> WebGpuRenderDevice::CreateSurface()
{
    if (surface != nullptr)
    {
        return {};
    }

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvasSource.selector = ToStringView("#canvas");
    WGPUSurfaceDescriptor surfaceDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surfaceDesc.nextInChain = &canvasSource.chain;
    surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (surface == nullptr)
    {
        return Error{"Could not create a WebGPU surface for #canvas"};
    }
    return ConfigureSurface();
}

void WebGpuRenderDevice::DestroySurface()
{
    if (surface == nullptr)
    {
        return;
    }
    wgpuSurfaceUnconfigure(surface);
    wgpuSurfaceRelease(surface);
    surface = nullptr;
    surfaceExtent = {};
}

bool WebGpuRenderDevice::HasSurface() const
{
    return surface != nullptr;
}

Result<void> WebGpuRenderDevice::RecreateSwapchain()
{
    if (surface == nullptr)
    {
        return {};
    }
    return ConfigureSurface();
}

Result<void> WebGpuRenderDevice::ConfigureSurface()
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(platform->GetNativeWindow(), &width, &height);
    if (width <= 0 || height <= 0)
    {
        surfaceExtent = {};
        return {};
    }

    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device;
    config.format = surfaceFormat;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = static_cast<uint32_t>(width);
    config.height = static_cast<uint32_t>(height);
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &config);
    surfaceExtent = {config.width, config.height};
    return {};
}

// --- Resources ---

Result<BufferHandle> WebGpuRenderDevice::CreateBuffer(const BufferDesc& desc)
{
    WGPUBufferDescriptor bufferDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    bufferDesc.label = ToStringView(desc.DebugName);
    bufferDesc.usage =
        (desc.Usage == BufferUsage::Index ? WGPUBufferUsage_Index : WGPUBufferUsage_Vertex) | WGPUBufferUsage_CopyDst;
    bufferDesc.size = AlignUp4(desc.Size);
    const WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &bufferDesc);
    if (buffer == nullptr)
    {
        return Error{std::string("wgpuDeviceCreateBuffer failed (") + desc.DebugName + ")"};
    }
    return buffers.Create({buffer, static_cast<size_t>(bufferDesc.size)});
}

Result<TextureHandle> WebGpuRenderDevice::CreateTexture(const TextureDesc& desc)
{
    const bool isR8 = desc.Format == TextureFormat::R8Unorm;
    WGPUTextureDescriptor textureDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    textureDesc.label = ToStringView(desc.DebugName);
    textureDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.size = {desc.Width, desc.Height, 1};
    // Vulkan reads R8 through a swizzle as white with the value in alpha; core WebGPU can't swizzle,
    // so R8 textures are stored that way instead (UpdateTexture expands the texels).
    textureDesc.format = WGPUTextureFormat_RGBA8Unorm;
    const WGPUTexture texture = wgpuDeviceCreateTexture(device, &textureDesc);
    if (texture == nullptr)
    {
        return Error{std::string("wgpuDeviceCreateTexture failed (") + desc.DebugName + ")"};
    }

    TextureData data;
    data.Texture = texture;
    data.View = wgpuTextureCreateView(texture, nullptr);
    data.Width = desc.Width;
    data.Height = desc.Height;
    data.IsExpandedR8 = isR8;
    return textures.Create(data);
}

Result<SamplerHandle> WebGpuRenderDevice::CreateSampler(const SamplerDesc& desc)
{
    const WGPUFilterMode filter = desc.MinMagFilter == Filter::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    const WGPUAddressMode address =
        desc.Address == AddressMode::Repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;

    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.addressModeU = address;
    samplerDesc.addressModeV = address;
    samplerDesc.addressModeW = address;
    samplerDesc.magFilter = filter;
    samplerDesc.minFilter = filter;
    const WGPUSampler sampler = wgpuDeviceCreateSampler(device, &samplerDesc);
    if (sampler == nullptr)
    {
        return Error{"wgpuDeviceCreateSampler failed"};
    }
    return samplers.Create({sampler});
}

void WebGpuRenderDevice::Destroy(BufferHandle buffer)
{
    // WebGPU keeps objects alive while submitted work uses them, so releasing now is safe.
    if (BufferData* data = buffers.Get(buffer))
    {
        wgpuBufferRelease(data->Buffer);
        buffers.Destroy(buffer);
    }
}

void WebGpuRenderDevice::Destroy(TextureHandle texture)
{
    if (TextureData* data = textures.Get(texture))
    {
        ForgetBindGroups(ToBits(texture.Index, texture.Generation), true);
        wgpuTextureViewRelease(data->View);
        wgpuTextureRelease(data->Texture);
        textures.Destroy(texture);
    }
}

void WebGpuRenderDevice::Destroy(SamplerHandle sampler)
{
    if (SamplerData* data = samplers.Get(sampler))
    {
        ForgetBindGroups(ToBits(sampler.Index, sampler.Generation), false);
        wgpuSamplerRelease(data->Sampler);
        samplers.Destroy(sampler);
    }
}

void WebGpuRenderDevice::Destroy(PipelineHandle pipeline)
{
    if (PipelineData* data = pipelines.Get(pipeline))
    {
        wgpuRenderPipelineRelease(data->Pipeline);
        pipelines.Destroy(pipeline);
    }
}

Result<void> WebGpuRenderDevice::UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset)
{
    const BufferData* bufferData = buffers.Get(buffer);
    if (bufferData == nullptr)
    {
        return Error{"UploadBuffer: invalid or destroyed buffer"};
    }
    if (size == 0)
    {
        return {};
    }
    if (offset % 4 != 0 || offset + AlignUp4(size) > bufferData->Size)
    {
        return Error{"UploadBuffer: the range is misaligned or past the end of the buffer"};
    }

    if (size % 4 == 0)
    {
        wgpuQueueWriteBuffer(queue, bufferData->Buffer, offset, data, size);
        return {};
    }
    // 16-bit indices can leave a size that isn't a multiple of 4: pad it.
    std::vector<std::byte> padded(AlignUp4(size));
    std::memcpy(padded.data(), data, size);
    wgpuQueueWriteBuffer(queue, bufferData->Buffer, offset, padded.data(), padded.size());
    return {};
}

Result<void> WebGpuRenderDevice::UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y,
                                               uint32_t width, uint32_t height)
{
    const TextureData* data = textures.Get(texture);
    if (data == nullptr)
    {
        return Error{"UpdateTexture: invalid or destroyed texture"};
    }
    if (width == 0 || height == 0)
    {
        return {};
    }

    std::vector<uint8_t> expanded;
    if (data->IsExpandedR8)
    {
        const size_t count = static_cast<size_t>(width) * height;
        const auto* values = static_cast<const uint8_t*>(pixels);
        expanded.resize(count * 4);
        for (size_t index = 0; index < count; ++index)
        {
            expanded[index * 4 + 0] = 255;
            expanded[index * 4 + 1] = 255;
            expanded[index * 4 + 2] = 255;
            expanded[index * 4 + 3] = values[index];
        }
        pixels = expanded.data();
    }

    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = data->Texture;
    destination.origin = {x, y, 0};
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = width * 4;
    layout.rowsPerImage = height;
    const WGPUExtent3D size{width, height, 1};
    wgpuQueueWriteTexture(queue, &destination, pixels, static_cast<size_t>(layout.bytesPerRow) * height, &layout,
                          &size);
    return {};
}

WGPUBindGroupLayout WebGpuRenderDevice::GetTextureLayout(uint32_t textureCount)
{
    if (auto found = textureLayouts.find(textureCount); found != textureLayouts.end())
    {
        return found->second;
    }

    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (uint32_t slot = 0; slot < textureCount; ++slot)
    {
        WGPUBindGroupLayoutEntry textureEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        textureEntry.binding = slot * 2;
        textureEntry.visibility = WGPUShaderStage_Fragment;
        textureEntry.texture.sampleType = WGPUTextureSampleType_Float;
        textureEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
        entries.push_back(textureEntry);

        WGPUBindGroupLayoutEntry samplerEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        samplerEntry.binding = slot * 2 + 1;
        samplerEntry.visibility = WGPUShaderStage_Fragment;
        samplerEntry.sampler.type = WGPUSamplerBindingType_Filtering;
        entries.push_back(samplerEntry);
    }

    WGPUBindGroupLayoutDescriptor layoutDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.label = ToStringView("textures");
    layoutDesc.entryCount = entries.size();
    layoutDesc.entries = entries.data();
    const WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDesc);
    textureLayouts.emplace(textureCount, layout);
    return layout;
}

WGPUBindGroup WebGpuRenderDevice::GetTextureBindGroup(uint32_t textureCount, const TextureHandle* boundTextures,
                                                      const SamplerHandle* boundSamplers)
{
    BindGroupEntry key;
    key.TextureCount = textureCount;
    uint64_t hash = 14695981039346656037ull ^ textureCount;
    for (uint32_t slot = 0; slot < textureCount; ++slot)
    {
        key.Textures[slot] = ToBits(boundTextures[slot].Index, boundTextures[slot].Generation);
        key.Samplers[slot] = ToBits(boundSamplers[slot].Index, boundSamplers[slot].Generation);
        hash = (hash ^ key.Textures[slot]) * 1099511628211ull;
        hash = (hash ^ key.Samplers[slot]) * 1099511628211ull;
    }

    std::vector<BindGroupEntry>& bucket = bindGroups[hash];
    for (const BindGroupEntry& entry : bucket)
    {
        if (entry.TextureCount == key.TextureCount && entry.Textures == key.Textures && entry.Samplers == key.Samplers)
        {
            return entry.BindGroup;
        }
    }

    std::vector<WGPUBindGroupEntry> entries;
    for (uint32_t slot = 0; slot < textureCount; ++slot)
    {
        const TextureData* textureData = textures.Get(boundTextures[slot]);
        const SamplerData* samplerData = samplers.Get(boundSamplers[slot]);
        if (textureData == nullptr || samplerData == nullptr)
        {
            HYOSHI_LOG_ERROR("A texture slot the pipeline uses is unbound or destroyed");
            return nullptr;
        }
        WGPUBindGroupEntry textureEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        textureEntry.binding = slot * 2;
        textureEntry.textureView = textureData->View;
        entries.push_back(textureEntry);
        WGPUBindGroupEntry samplerEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        samplerEntry.binding = slot * 2 + 1;
        samplerEntry.sampler = samplerData->Sampler;
        entries.push_back(samplerEntry);
    }

    WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    groupDesc.layout = GetTextureLayout(textureCount);
    groupDesc.entryCount = entries.size();
    groupDesc.entries = entries.data();
    key.BindGroup = wgpuDeviceCreateBindGroup(device, &groupDesc);
    bucket.push_back(key);
    return key.BindGroup;
}

void WebGpuRenderDevice::ForgetBindGroups(uint64_t handleBits, bool isTexture)
{
    for (auto& [hash, bucket] : bindGroups)
    {
        std::erase_if(bucket,
                      [&](const BindGroupEntry& entry)
                      {
                          const auto& handles = isTexture ? entry.Textures : entry.Samplers;
                          const auto end = handles.begin() + static_cast<std::ptrdiff_t>(entry.TextureCount);
                          const bool uses = std::find(handles.begin(), end, handleBits) != end;
                          if (uses)
                          {
                              wgpuBindGroupRelease(entry.BindGroup);
                          }
                          return uses;
                      });
    }
}

uint32_t WebGpuRenderDevice::PushConstantData(const void* data, uint32_t size)
{
    HYOSHI_ASSERT(size <= CONSTANT_SLOT_SIZE);
    if (nextConstantSlot >= CONSTANT_SLOTS)
    {
        HYOSHI_LOG_ERROR("More than {} constant pushes in a frame; reusing the last slot", CONSTANT_SLOTS);
        nextConstantSlot = CONSTANT_SLOTS - 1;
    }
    const uint32_t offset = nextConstantSlot * CONSTANT_SLOT_SIZE;
    ++nextConstantSlot;

    std::array<std::byte, CONSTANT_SLOT_SIZE> slot{};
    std::memcpy(slot.data(), data, size);
    wgpuQueueWriteBuffer(queue, constantBuffer, offset, slot.data(), AlignUp4(size));
    return offset;
}

// --- Pipelines ---

Result<PipelineHandle> WebGpuRenderDevice::CreatePipeline(const PipelineDesc& desc)
{
    if (desc.Wgsl.empty())
    {
        return Error{std::string("No WGSL for pipeline ") + desc.DebugName};
    }

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {desc.Wgsl.data(), desc.Wgsl.size()};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = ToStringView(desc.DebugName);
    const WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &moduleDesc);

    // Vertex buffers sit at their binding numbers; slots in between stay unused.
    uint32_t bufferCount = 0;
    for (const VertexBindingDesc& binding : desc.VertexBindings)
    {
        bufferCount = std::max(bufferCount, binding.Binding + 1);
    }
    std::vector<std::vector<WGPUVertexAttribute>> attributes(bufferCount);
    for (const VertexAttributeDesc& attribute : desc.VertexAttributes)
    {
        WGPUVertexAttribute vertexAttribute = WGPU_VERTEX_ATTRIBUTE_INIT;
        vertexAttribute.format = ToWgpuFormat(attribute.Format);
        vertexAttribute.offset = attribute.Offset;
        vertexAttribute.shaderLocation = attribute.Location;
        attributes[attribute.Binding].push_back(vertexAttribute);
    }
    std::vector<WGPUVertexBufferLayout> layouts(bufferCount);
    for (uint32_t index = 0; index < bufferCount; ++index)
    {
        layouts[index] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    }
    for (const VertexBindingDesc& binding : desc.VertexBindings)
    {
        WGPUVertexBufferLayout& layout = layouts[binding.Binding];
        layout.stepMode =
            binding.InputRate == VertexInputRate::Instance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        layout.arrayStride = binding.Stride;
        layout.attributeCount = attributes[binding.Binding].size();
        layout.attributes = attributes[binding.Binding].data();
    }

    const bool hasConstants = desc.PushConstantSize > 0;
    std::vector<WGPUBindGroupLayout> groupLayouts;
    if (desc.TextureCount > 0 || hasConstants)
    {
        groupLayouts.push_back(GetTextureLayout(desc.TextureCount));
    }
    if (hasConstants)
    {
        groupLayouts.push_back(constantLayout);
    }
    WGPUPipelineLayoutDescriptor pipelineLayoutDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pipelineLayoutDesc.bindGroupLayoutCount = groupLayouts.size();
    pipelineLayoutDesc.bindGroupLayouts = groupLayouts.data();
    const WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDesc);

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = surfaceFormat;
    target.blend = desc.Blend == BlendMode::Alpha ? &blend : nullptr;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = ToStringView(desc.FragmentEntry);
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = ToStringView(desc.DebugName);
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = ToStringView(desc.VertexEntry);
    pipelineDesc.vertex.bufferCount = layouts.size();
    pipelineDesc.vertex.buffers = layouts.data();
    pipelineDesc.primitive.topology = desc.Topology == PrimitiveTopology::TriangleStrip
                                          ? WGPUPrimitiveTopology_TriangleStrip
                                          : WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.primitive.frontFace = WGPUFrontFace_CCW;
    pipelineDesc.primitive.cullMode = WGPUCullMode_None;
    pipelineDesc.fragment = &fragment;

    const WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &pipelineDesc);
    wgpuPipelineLayoutRelease(pipelineLayout);
    wgpuShaderModuleRelease(module);
    if (pipeline == nullptr)
    {
        return Error{std::string("wgpuDeviceCreateRenderPipeline failed (") + desc.DebugName + ")"};
    }

    PipelineData data;
    data.Pipeline = pipeline;
    data.TextureCount = desc.TextureCount;
    data.HasConstants = hasConstants;
    return pipelines.Create(data);
}

// --- Frames ---

FrameStatus WebGpuRenderDevice::BeginFrame()
{
    HYOSHI_ASSERT(!isFrameActive, "BeginFrame called twice");
    if (surface == nullptr)
    {
        return FrameStatus::Skipped;
    }

    // The canvas follows the window; reconfigure when its pixel size changes.
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(platform->GetNativeWindow(), &width, &height);
    if (width <= 0 || height <= 0)
    {
        return FrameStatus::Skipped;
    }
    if (static_cast<uint32_t>(width) != surfaceExtent.Width || static_cast<uint32_t>(height) != surfaceExtent.Height)
    {
        if (Result<void> result = ConfigureSurface(); !result)
        {
            HYOSHI_LOG_ERROR("Could not configure the canvas: {}", result.GetError().Message);
            return FrameStatus::Skipped;
        }
    }

    currentTexture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface, &currentTexture);
    if (currentTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        currentTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
    {
        if (currentTexture.texture != nullptr)
        {
            wgpuTextureRelease(currentTexture.texture);
        }
        if (Result<void> result = ConfigureSurface(); !result)
        {
            HYOSHI_LOG_ERROR("Could not configure the canvas: {}", result.GetError().Message);
        }
        return FrameStatus::Skipped;
    }
    currentView = wgpuTextureCreateView(currentTexture.texture, nullptr);

    nextConstantSlot = 0;
    WGPUCommandEncoderDescriptor encoderDesc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    commandList.Reset(wgpuDeviceCreateCommandEncoder(device, &encoderDesc));
    isFrameActive = true;
    return FrameStatus::Ready;
}

ICommandList& WebGpuRenderDevice::GetCommandList()
{
    HYOSHI_ASSERT(isFrameActive, "GetCommandList is only valid between BeginFrame and EndFrameAndPresent");
    return commandList;
}

void WebGpuRenderDevice::EndFrameAndPresent()
{
    HYOSHI_ASSERT(isFrameActive);
    commandList.EndOpenPass();

    WGPUCommandBufferDescriptor bufferDesc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    const WGPUCommandBuffer commands = wgpuCommandEncoderFinish(commandList.encoder, &bufferDesc);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(commandList.encoder);
    commandList.encoder = nullptr;

    // The browser presents the canvas when control returns to it; there's no present call.
    wgpuTextureViewRelease(currentView);
    currentView = nullptr;
    wgpuTextureRelease(currentTexture.texture);
    currentTexture = WGPU_SURFACE_TEXTURE_INIT;

    isFrameActive = false;
    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
}

uint32_t WebGpuRenderDevice::GetFrameIndex() const
{
    return frameIndex;
}

Extent WebGpuRenderDevice::GetSwapchainExtent() const
{
    return surfaceExtent;
}

SurfaceTransform WebGpuRenderDevice::GetSurfaceTransform() const
{
    return SurfaceTransform::Identity;
}

void WebGpuRenderDevice::SavePipelineCache()
{
}

uint32_t WebGpuRenderDevice::GetValidationMessageCount() const
{
    return static_cast<uint32_t>(GetWebGpuErrorCount());
}

void WebGpuRenderDevice::RequestScreenshot(std::string path)
{
    HYOSHI_LOG_WARN("Screenshots aren't supported on the web ('{}')", path);
}

} // namespace hyoshi::rhi
