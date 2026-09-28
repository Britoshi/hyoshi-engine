#pragma once

#include "core/Result.h"
#include "rhi/RhiTypes.h"

#include <memory>
#include <string>

namespace hyoshi::platform
{
class Platform;
}

namespace hyoshi::rhi
{

// Records rendering commands for the current frame. Valid between BeginFrame and EndFrameAndPresent.
class ICommandList
{
public:
    virtual ~ICommandList() = default;

    // Also sets the viewport and scissor to cover the whole target.
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    virtual void BindPipeline(PipelineHandle pipeline) = 0;
    virtual void BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type) = 0;
    // Takes effect at the next draw. Slots are the pipeline's texture bindings.
    virtual void BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) = 0;
    virtual void PushConstants(const void* data, uint32_t size) = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const Rect& scissor) = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                             uint32_t firstInstance) = 0;
};

// Backend-agnostic GPU device (DESIGN.md section 10.1). This is the subset needed so far; it grows
// with real needs and is finalized in M2.
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;

    virtual Result<void> Initialize(const DeviceConfig& config, platform::Platform& platform) = 0;
    virtual void Shutdown() = 0;

    // Surface lifecycle. Android destroys the native window in the background, so the surface and
    // swapchain are released there and recreated on return.
    virtual Result<void> CreateSurface() = 0;
    virtual void DestroySurface() = 0;
    virtual bool HasSurface() const = 0;
    virtual Result<void> RecreateSwapchain() = 0;

    // Resources. Destroy is deferred until frames in flight no longer use the resource.
    virtual Result<BufferHandle> CreateBuffer(const BufferDesc& desc) = 0;
    virtual Result<TextureHandle> CreateTexture(const TextureDesc& desc) = 0;
    virtual Result<SamplerHandle> CreateSampler(const SamplerDesc& desc) = 0;
    virtual Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) = 0;
    virtual void Destroy(BufferHandle buffer) = 0;
    virtual void Destroy(TextureHandle texture) = 0;
    virtual void Destroy(SamplerHandle sampler) = 0;
    virtual void Destroy(PipelineHandle pipeline) = 0;

    // CpuToGpu buffers: copies into mapped memory. GpuOnly buffers: uploads through a staging
    // buffer and waits for the copy, so keep it to load time.
    virtual Result<void> UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset) = 0;

    // Replaces a rectangle of texels (tightly packed rows). Waits for the copy; safe mid-frame.
    virtual Result<void> UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height) = 0;

    virtual FrameStatus BeginFrame() = 0;
    virtual ICommandList& GetCommandList() = 0;
    virtual void EndFrameAndPresent() = 0;

    // 0..MAX_FRAMES_IN_FLIGHT-1: which copy of per-frame data the current frame may write.
    virtual uint32_t GetFrameIndex() const = 0;

    virtual Extent GetSwapchainExtent() const = 0;
    virtual SurfaceTransform GetSurfaceTransform() const = 0;

    // Writes the pipeline cache to DeviceConfig::PipelineCachePath now (also done on Shutdown).
    virtual void SavePipelineCache() = 0;

    // Debugging: warnings and errors reported by the validation layer so far.
    virtual uint32_t GetValidationMessageCount() const = 0;

    // Debugging: saves the next presented frame as a PNG.
    virtual void RequestScreenshot(std::string path) = 0;
};

// Creates the backend chosen at build time (Vulkan).
std::unique_ptr<IRenderDevice> CreateRenderDevice();

} // namespace hyoshi::rhi
