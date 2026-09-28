#pragma once

#include "VulkanCommon.h"
#include "VulkanPipelineCache.h"
#include "VulkanSwapchain.h"

#include "core/Handle.h"
#include "rhi/IRenderDevice.h"

#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyoshi::rhi
{

class VulkanRenderDevice;

class VulkanCommandList final : public ICommandList
{
public:
    explicit VulkanCommandList(VulkanRenderDevice& owner) : device(owner)
    {
    }

    void Reset(VkCommandBuffer commandBuffer);

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

    static constexpr uint32_t MAX_TEXTURE_SLOTS = 4;

private:
    // Writes and binds a descriptor set for the textures bound since the last draw.
    void FlushTextures();

    VulkanRenderDevice& device;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkPipelineLayout boundLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout boundSetLayout = VK_NULL_HANDLE;
    uint32_t boundTextureCount = 0;
    std::array<VkDescriptorImageInfo, MAX_TEXTURE_SLOTS> boundTextures{};
    bool areTexturesDirty = false;
    bool isInRenderPass = false;
};

class VulkanRenderDevice final : public IRenderDevice
{
public:
    VulkanRenderDevice();
    ~VulkanRenderDevice() override;

    VulkanRenderDevice(const VulkanRenderDevice&) = delete;
    VulkanRenderDevice& operator=(const VulkanRenderDevice&) = delete;

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
    friend class VulkanCommandList;

    struct BufferData
    {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VmaAllocation Allocation = VK_NULL_HANDLE;
        size_t Size = 0;
        BufferUsage Usage = BufferUsage::Vertex;
        MemoryUsage Memory = MemoryUsage::GpuOnly;
    };

    struct TextureData
    {
        VkImage Image = VK_NULL_HANDLE;
        VmaAllocation Allocation = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t BytesPerPixel = 4;
    };

    struct SamplerData
    {
        VkSampler Sampler = VK_NULL_HANDLE;
    };

    struct PipelineData
    {
        VkPipeline Pipeline = VK_NULL_HANDLE;
        VkPipelineLayout Layout = VK_NULL_HANDLE;
        // Shared, owned by setLayoutsByTextureCount.
        VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
        uint32_t TextureCount = 0;
    };

    struct FrameData
    {
        VkCommandPool CommandPool = VK_NULL_HANDLE;
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        VkSemaphore ImageAvailable = VK_NULL_HANDLE;
        VkFence InFlight = VK_NULL_HANDLE;
        // Descriptor sets live for one frame; pools are reset when the frame slot comes around.
        std::vector<VkDescriptorPool> DescriptorPools;
        uint32_t ActiveDescriptorPool = 0;
        // Which submission last used this slot (see completedSubmissions).
        uint64_t SubmissionNumber = 0;
    };

    // A destroyed resource is freed once the last submission that could reference it completes.
    struct PendingDeletion
    {
        uint64_t LastUsingSubmission = 0;
        std::function<void()> Delete;
    };

    static constexpr uint32_t FRAMES_IN_FLIGHT = MAX_FRAMES_IN_FLIGHT;

    static VkBool32 VKAPI_PTR OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData);

    Result<void> CreateInstance(const DeviceConfig& config);
    Result<void> SelectPhysicalDevice();
    Result<void> CreateLogicalDevice();
    Result<void> CreateAllocator();
    Result<void> CreateFrameResources();
    Result<void> CreateSwapchainResources();
    Result<void> CreateSwapchainRenderPass(VkFormat format);
    void RecordScreenshotCopy(VkCommandBuffer commandBuffer, VkImage image);
    void WritePendingScreenshot();

    Result<void> SubmitImmediate(const std::function<void(VkCommandBuffer)>& record);
    Result<VkDescriptorSetLayout> GetSetLayout(uint32_t textureCount);
    VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
    void DeferDeletion(std::function<void()> deletion);
    void RunPendingDeletions();
    void MarkAllSubmissionsComplete();
    void DestroyAllResources();

    platform::Platform* platform = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProperties{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    bool hasPortabilitySubset = false;

    VulkanSwapchain swapchain;
    VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
    VkFormat renderPassFormat = VK_FORMAT_UNDEFINED;
    bool isSwapchainDirty = false;

    std::array<FrameData, FRAMES_IN_FLIGHT> frames{};
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool isFrameActive = false;

    VulkanCommandList commandList;
    HandlePool<BufferData, BufferTag> buffers;
    HandlePool<TextureData, TextureTag> textures;
    HandlePool<SamplerData, SamplerTag> samplers;
    HandlePool<PipelineData, PipelineTag> pipelines;
    std::unordered_map<uint32_t, VkDescriptorSetLayout> setLayoutsByTextureCount;
    VulkanPipelineCache pipelineCache;

    uint64_t submittedCount = 0;
    uint64_t completedSubmissions = 0;
    std::vector<PendingDeletion> pendingDeletions;

    VkCommandPool immediatePool = VK_NULL_HANDLE;
    VkCommandBuffer immediateCommandBuffer = VK_NULL_HANDLE;
    VkFence immediateFence = VK_NULL_HANDLE;

    std::atomic<uint32_t> validationMessageCount = 0;

    // Screenshot readback: requested -> copy recorded into a frame -> written after that frame.
    std::string screenshotPath;
    bool isScreenshotRequested = false;
    bool isScreenshotRecorded = false;
    VkBuffer screenshotBuffer = VK_NULL_HANDLE;
    VmaAllocation screenshotAllocation = VK_NULL_HANDLE;
    VkExtent2D screenshotExtent{};
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;

    bool isInitialized = false;
};

} // namespace hyoshi::rhi
