#pragma once

#include "core/Handle.h"
#include "rhi/IRenderDevice.h"

#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyoshi::rhi
{

class WebGpuRenderDevice;

class WebGpuCommandList final : public ICommandList
{
public:
    explicit WebGpuCommandList(WebGpuRenderDevice& owner) : device(owner)
    {
    }

    void Reset(WGPUCommandEncoder newEncoder);

    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;
    void BindPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset) override;
    void BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type) override;
    void BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
    void PushConstants(const void* data, uint32_t size) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance) override;

    // Ends a render pass left open, so the encoder can finish.
    void EndOpenPass();

    static constexpr uint32_t MAX_TEXTURE_SLOTS = 4;

private:
    friend class WebGpuRenderDevice;

    // Binds the textures bound since the last draw (group 0) and the latest constants (group 1).
    void FlushBindings();

    WebGpuRenderDevice& device;
    WGPUCommandEncoder encoder = nullptr;
    WGPURenderPassEncoder pass = nullptr;
    uint32_t boundTextureCount = 0;
    bool hasConstants = false;
    std::array<TextureHandle, MAX_TEXTURE_SLOTS> boundTextures{};
    std::array<SamplerHandle, MAX_TEXTURE_SLOTS> boundSamplers{};
    bool areTexturesDirty = false;
    bool areConstantsDirty = false;
    uint32_t constantsOffset = 0;
};

// The browser's GPU, through webgpu.h (Emscripten's emdawnwebgpu port). Push constants become a
// uniform ring buffer at group 1 with a dynamic offset per push; texture slot N is the texture at
// group 0 binding 2N and its sampler at 2N+1, which is how Slang lays out [[vk::binding(N, 0)]]
// Sampler2D for WGSL when N is 0. The device is created by the page before the module starts
// (Module.preinitializedWebGPUDevice), since adapter and device requests are asynchronous.
class WebGpuRenderDevice final : public IRenderDevice
{
public:
    WebGpuRenderDevice();
    ~WebGpuRenderDevice() override;

    WebGpuRenderDevice(const WebGpuRenderDevice&) = delete;
    WebGpuRenderDevice& operator=(const WebGpuRenderDevice&) = delete;

    Result<void> Initialize(const DeviceConfig& config, platform::Platform& platform) override;
    void Shutdown() override;

    Result<void> CreateSurface() override;
    void DestroySurface() override;
    bool HasSurface() const override;
    Result<void> RecreateSwapchain() override;

    Result<BufferHandle> CreateBuffer(const BufferDesc& desc) override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc) override;
    Result<SamplerHandle> CreateSampler(const SamplerDesc& desc) override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) override;
    void Destroy(BufferHandle buffer) override;
    void Destroy(TextureHandle texture) override;
    void Destroy(SamplerHandle sampler) override;
    void Destroy(PipelineHandle pipeline) override;

    Result<void> UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset) override;
    Result<void> UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height) override;

    FrameStatus BeginFrame() override;
    ICommandList& GetCommandList() override;
    void EndFrameAndPresent() override;

    uint32_t GetFrameIndex() const override;
    Extent GetSwapchainExtent() const override;
    SurfaceTransform GetSurfaceTransform() const override;

    void SavePipelineCache() override;
    uint32_t GetValidationMessageCount() const override;
    void RequestScreenshot(std::string path) override;

private:
    friend class WebGpuCommandList;

    struct BufferData
    {
        WGPUBuffer Buffer = nullptr;
        size_t Size = 0;
    };

    struct TextureData
    {
        WGPUTexture Texture = nullptr;
        WGPUTextureView View = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        // R8 textures are stored as RGBA8 (white, the value in alpha): WebGPU has no view swizzle.
        bool IsExpandedR8 = false;
    };

    struct SamplerData
    {
        WGPUSampler Sampler = nullptr;
    };

    struct PipelineData
    {
        WGPURenderPipeline Pipeline = nullptr;
        uint32_t TextureCount = 0;
        bool HasConstants = false;
    };

    // Every push in a frame gets its own slot; the ring starts over each frame, since queue writes
    // and submits run in order.
    static constexpr uint32_t CONSTANT_SLOT_SIZE = 256;
    static constexpr uint32_t CONSTANT_SLOTS = 1024;

    Result<void> ConfigureSurface();
    WGPUBindGroupLayout GetTextureLayout(uint32_t textureCount);
    WGPUBindGroup GetTextureBindGroup(uint32_t textureCount, const TextureHandle* textures,
                                      const SamplerHandle* samplers);
    // Writes the constants into the next slot and returns its offset.
    uint32_t PushConstantData(const void* data, uint32_t size);
    void ForgetBindGroups(uint64_t handleBits, bool isTexture);

    platform::Platform* platform = nullptr;

    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
    Extent surfaceExtent;

    WGPUBuffer constantBuffer = nullptr;
    WGPUBindGroupLayout constantLayout = nullptr;
    WGPUBindGroup constantBindGroup = nullptr;
    uint32_t nextConstantSlot = 0;

    std::unordered_map<uint32_t, WGPUBindGroupLayout> textureLayouts;
    // Keyed by the texture count and the bound texture and sampler handles.
    struct BindGroupEntry
    {
        std::array<uint64_t, WebGpuCommandList::MAX_TEXTURE_SLOTS> Textures{};
        std::array<uint64_t, WebGpuCommandList::MAX_TEXTURE_SLOTS> Samplers{};
        uint32_t TextureCount = 0;
        WGPUBindGroup BindGroup = nullptr;
    };
    std::unordered_map<uint64_t, std::vector<BindGroupEntry>> bindGroups;

    WGPUSurfaceTexture currentTexture{};
    WGPUTextureView currentView = nullptr;
    uint32_t frameIndex = 0;
    bool isFrameActive = false;

    WebGpuCommandList commandList;
    HandlePool<BufferData, BufferTag> buffers;
    HandlePool<TextureData, TextureTag> textures;
    HandlePool<SamplerData, SamplerTag> samplers;
    HandlePool<PipelineData, PipelineTag> pipelines;

    bool isInitialized = false;
};

} // namespace hyoshi::rhi
