// VulkanRenderDevice: buffers, textures, samplers, descriptor sets, uploads, and deferred deletion.

#include "VulkanRenderDevice.h"

#include "core/Log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <vector>

namespace hyoshi::rhi
{

namespace
{

// Sized for a frame of sprites and debug UI; another pool is added if a frame needs more.
constexpr uint32_t DESCRIPTOR_POOL_SETS = 512;
constexpr uint32_t DESCRIPTOR_POOL_SAMPLERS = 1024;
constexpr uint32_t DESCRIPTOR_POOL_STORAGE_BUFFERS = 256;

VkFormat ToVkFormat(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::R8Unorm:
        return VK_FORMAT_R8_UNORM;
    case TextureFormat::Rgba8Unorm:
        break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

uint32_t BytesPerPixel(TextureFormat format)
{
    return format == TextureFormat::R8Unorm ? 1 : 4;
}

VkFilter ToVkFilter(Filter filter)
{
    return filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode ToVkAddressMode(AddressMode mode)
{
    return mode == AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

// A buffer the CPU writes once and the GPU copies from.
struct StagingBuffer
{
    VkBuffer Buffer = VK_NULL_HANDLE;
    VmaAllocation Allocation = VK_NULL_HANDLE;
};

Result<StagingBuffer> CreateStagingBuffer(VmaAllocator allocator, const void* data, size_t size)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    StagingBuffer staging;
    if (Result<void> result = CheckVk(
            vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &staging.Buffer, &staging.Allocation, nullptr),
            "vmaCreateBuffer (staging)");
        !result)
    {
        return result.GetError();
    }

    if (Result<void> result = CheckVk(vmaCopyMemoryToAllocation(allocator, data, staging.Allocation, 0, size),
                                      "vmaCopyMemoryToAllocation");
        !result)
    {
        vmaDestroyBuffer(allocator, staging.Buffer, staging.Allocation);
        return result.GetError();
    }

    return staging;
}

} // namespace

Result<BufferHandle> VulkanRenderDevice::CreateBuffer(const BufferDesc& desc)
{
    HYOSHI_ASSERT(desc.Size > 0);

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = desc.Size;
    switch (desc.Usage)
    {
    case BufferUsage::Index:
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
    case BufferUsage::Storage:
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    case BufferUsage::Vertex:
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    }
    if (desc.Memory == MemoryUsage::GpuOnly)
    {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc.Memory == MemoryUsage::CpuToGpu)
    {
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    BufferData data;
    data.Size = desc.Size;
    data.Usage = desc.Usage;
    data.Memory = desc.Memory;
    if (Result<void> result =
            CheckVk(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &data.Buffer, &data.Allocation, nullptr),
                    "vmaCreateBuffer");
        !result)
    {
        return Error{result.GetError().Message + " (" + desc.DebugName + ")"};
    }

    return buffers.Create(data);
}

Result<TextureHandle> VulkanRenderDevice::CreateTexture(const TextureDesc& desc)
{
    HYOSHI_ASSERT(desc.Width > 0 && desc.Height > 0);

    const VkFormat format = ToVkFormat(desc.Format);
    // Down to 1 x 1 at most.
    const uint32_t mipLevels =
        std::clamp(desc.MipLevels, 1U, static_cast<uint32_t>(std::bit_width(std::max(desc.Width, desc.Height))));

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {desc.Width, desc.Height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    TextureData data;
    data.Width = desc.Width;
    data.Height = desc.Height;
    data.BytesPerPixel = BytesPerPixel(desc.Format);
    data.MipLevels = mipLevels;
    if (Result<void> result =
            CheckVk(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &data.Image, &data.Allocation, nullptr),
                    "vmaCreateImage");
        !result)
    {
        return Error{result.GetError().Message + " (" + desc.DebugName + ")"};
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = data.Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    if (desc.Format == TextureFormat::R8Unorm)
    {
        // Single-channel textures read as white with the value in alpha (glyphs, masks).
        viewInfo.components = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
                               VK_COMPONENT_SWIZZLE_R};
    }

    Result<void> result = CheckVk(vkCreateImageView(device, &viewInfo, nullptr, &data.View), "vkCreateImageView");

    // Invariant: outside uploads, every texture is in SHADER_READ_ONLY_OPTIMAL.
    if (result)
    {
        result = SubmitImmediate(
            [&data](VkCommandBuffer commandBuffer)
            {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = data.Image;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, data.MipLevels, 0, 1};
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            });
    }

    if (!result)
    {
        if (data.View != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, data.View, nullptr);
        }
        vmaDestroyImage(allocator, data.Image, data.Allocation);
        return Error{result.GetError().Message + " (" + desc.DebugName + ")"};
    }

    return textures.Create(data);
}

Result<SamplerHandle> VulkanRenderDevice::CreateSampler(const SamplerDesc& desc)
{
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = ToVkFilter(desc.MinMagFilter);
    samplerInfo.minFilter = ToVkFilter(desc.MinMagFilter);
    // Between a texture's levels (TextureDesc::MipLevels), where it has them.
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = ToVkAddressMode(desc.Address);
    samplerInfo.addressModeV = ToVkAddressMode(desc.Address);
    samplerInfo.addressModeW = ToVkAddressMode(desc.Address);
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    const float anisotropy = std::min(desc.MaxAnisotropy, maxAnisotropy);
    samplerInfo.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = std::max(anisotropy, 1.0f);

    SamplerData data;
    if (Result<void> result = CheckVk(vkCreateSampler(device, &samplerInfo, nullptr, &data.Sampler), "vkCreateSampler");
        !result)
    {
        return result.GetError();
    }
    return samplers.Create(data);
}

void VulkanRenderDevice::Destroy(BufferHandle buffer)
{
    const BufferData* data = buffers.Get(buffer);
    if (data == nullptr)
    {
        return;
    }
    DeferDeletion([vmaAllocator = allocator, vkBuffer = data->Buffer, allocation = data->Allocation]
                  { vmaDestroyBuffer(vmaAllocator, vkBuffer, allocation); });
    buffers.Destroy(buffer);
}

void VulkanRenderDevice::Destroy(TextureHandle texture)
{
    const TextureData* data = textures.Get(texture);
    if (data == nullptr)
    {
        return;
    }
    DeferDeletion(
        [vkDevice = device, vmaAllocator = allocator, image = data->Image, allocation = data->Allocation,
         view = data->View]
        {
            vkDestroyImageView(vkDevice, view, nullptr);
            vmaDestroyImage(vmaAllocator, image, allocation);
        });
    textures.Destroy(texture);
}

void VulkanRenderDevice::Destroy(SamplerHandle sampler)
{
    const SamplerData* data = samplers.Get(sampler);
    if (data == nullptr)
    {
        return;
    }
    DeferDeletion([vkDevice = device, vkSampler = data->Sampler] { vkDestroySampler(vkDevice, vkSampler, nullptr); });
    samplers.Destroy(sampler);
}

Result<void> VulkanRenderDevice::UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset)
{
    const BufferData* bufferData = buffers.Get(buffer);
    if (bufferData == nullptr)
    {
        return Error{"UploadBuffer: invalid or destroyed buffer"};
    }
    HYOSHI_ASSERT(offset + size <= bufferData->Size, "UploadBuffer writes past the end of the buffer");
    if (size == 0)
    {
        return {};
    }

    if (bufferData->Memory == MemoryUsage::CpuToGpu)
    {
        return CheckVk(vmaCopyMemoryToAllocation(allocator, data, bufferData->Allocation, offset, size),
                       "vmaCopyMemoryToAllocation");
    }

    Result<StagingBuffer> staging = CreateStagingBuffer(allocator, data, size);
    if (!staging)
    {
        return staging.GetError();
    }

    const VkBuffer source = staging.Value().Buffer;
    const VkBuffer destination = bufferData->Buffer;
    const bool isStorage = bufferData->Usage == BufferUsage::Storage;
    VkAccessFlags readAccess = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    if (bufferData->Usage == BufferUsage::Index)
    {
        readAccess = VK_ACCESS_INDEX_READ_BIT;
    }
    else if (isStorage)
    {
        readAccess = VK_ACCESS_SHADER_READ_BIT;
    }
    const VkPipelineStageFlags readStage =
        isStorage ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    Result<void> result = SubmitImmediate(
        [&](VkCommandBuffer commandBuffer)
        {
            const VkBufferCopy region{0, offset, size};
            vkCmdCopyBuffer(commandBuffer, source, destination, 1, &region);

            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = readAccess;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = destination;
            barrier.offset = offset;
            barrier.size = size;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, readStage, 0, 0, nullptr, 1, &barrier,
                                 0, nullptr);
        });

    vmaDestroyBuffer(allocator, staging.Value().Buffer, staging.Value().Allocation);
    return result;
}

Result<void> VulkanRenderDevice::UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y,
                                               uint32_t width, uint32_t height)
{
    const TextureData* data = textures.Get(texture);
    if (data == nullptr)
    {
        return Error{"UpdateTexture: invalid or destroyed texture"};
    }
    HYOSHI_ASSERT(x + width <= data->Width && y + height <= data->Height, "UpdateTexture region is out of bounds");
    return CopyIntoTexture(*data, pixels, 0, x, y, width, height);
}

Result<void> VulkanRenderDevice::UpdateTextureLevel(TextureHandle texture, uint32_t level, const void* pixels)
{
    const TextureData* data = textures.Get(texture);
    if (data == nullptr)
    {
        return Error{"UpdateTextureLevel: invalid or destroyed texture"};
    }
    if (level >= data->MipLevels)
    {
        return {};
    }
    return CopyIntoTexture(*data, pixels, level, 0, 0, std::max(data->Width >> level, 1U),
                           std::max(data->Height >> level, 1U));
}

Result<void> VulkanRenderDevice::CopyIntoTexture(const TextureData& data, const void* pixels, uint32_t level,
                                                 uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return {};
    }

    const size_t size = size_t{width} * height * data.BytesPerPixel;
    Result<StagingBuffer> staging = CreateStagingBuffer(allocator, pixels, size);
    if (!staging)
    {
        return staging.GetError();
    }

    const VkBuffer source = staging.Value().Buffer;
    const VkImage image = data.Image;
    Result<void> result = SubmitImmediate(
        [&](VkCommandBuffer commandBuffer)
        {
            // Earlier frames may still sample the texture: wait for their fragment shaders
            // (earlier submissions on this queue), then write.
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image;
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
            region.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(commandBuffer, source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier toShader = toTransfer;
            toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toShader);
        });

    vmaDestroyBuffer(allocator, staging.Value().Buffer, staging.Value().Allocation);
    return result;
}

Result<void> VulkanRenderDevice::SubmitImmediate(const std::function<void(VkCommandBuffer)>& record)
{
    vkResetCommandBuffer(immediateCommandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(immediateCommandBuffer, &beginInfo);
    record(immediateCommandBuffer);
    vkEndCommandBuffer(immediateCommandBuffer);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &immediateCommandBuffer;

    vkResetFences(device, 1, &immediateFence);
    if (Result<void> result = CheckVk(vkQueueSubmit(queue, 1, &submitInfo, immediateFence), "vkQueueSubmit"); !result)
    {
        return result;
    }
    return CheckVk(vkWaitForFences(device, 1, &immediateFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

Result<VkDescriptorSetLayout> VulkanRenderDevice::GetSetLayout(uint32_t textureCount, uint32_t storageBufferCount)
{
    HYOSHI_ASSERT(textureCount <= VulkanCommandList::MAX_TEXTURE_SLOTS);
    HYOSHI_ASSERT(storageBufferCount <= VulkanCommandList::MAX_STORAGE_BUFFER_SLOTS);

    const uint32_t key = textureCount + (storageBufferCount << 8U);
    if (auto found = setLayouts.find(key); found != setLayouts.end())
    {
        return found->second;
    }

    std::array<VkDescriptorSetLayoutBinding,
               VulkanCommandList::MAX_TEXTURE_SLOTS + VulkanCommandList::MAX_STORAGE_BUFFER_SLOTS>
        bindings{};
    const uint32_t count = textureCount + storageBufferCount;
    for (uint32_t binding = 0; binding < count; ++binding)
    {
        const bool isTexture = binding < textureCount;
        bindings[binding].binding = binding;
        bindings[binding].descriptorType =
            isTexture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = isTexture ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    }

    VkDescriptorSetLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    createInfo.bindingCount = count;
    createInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (Result<void> result =
            CheckVk(vkCreateDescriptorSetLayout(device, &createInfo, nullptr, &layout), "vkCreateDescriptorSetLayout");
        !result)
    {
        return result.GetError();
    }

    setLayouts.emplace(key, layout);
    return layout;
}

VkDescriptorSet VulkanRenderDevice::AllocateDescriptorSet(VkDescriptorSetLayout layout)
{
    FrameData& frame = frames[frameIndex];

    while (true)
    {
        if (frame.ActiveDescriptorPool == frame.DescriptorPools.size())
        {
            const std::array<VkDescriptorPoolSize, 2> poolSizes{{
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DESCRIPTOR_POOL_SAMPLERS},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DESCRIPTOR_POOL_STORAGE_BUFFERS},
            }};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = DESCRIPTOR_POOL_SETS;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();

            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            {
                HYOSHI_LOG_ERROR("Could not create a descriptor pool");
                return VK_NULL_HANDLE;
            }
            frame.DescriptorPools.push_back(pool);
        }

        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = frame.DescriptorPools[frame.ActiveDescriptorPool];
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(device, &allocateInfo, &set);
        if (result == VK_SUCCESS)
        {
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            HYOSHI_LOG_ERROR("vkAllocateDescriptorSets failed: {}", ToString(result));
            return VK_NULL_HANDLE;
        }
        ++frame.ActiveDescriptorPool;
    }
}

void VulkanRenderDevice::DeferDeletion(std::function<void()> deletion)
{
    // The frame being recorded (if any) is the next submission; otherwise the last one submitted.
    const uint64_t lastUsingSubmission = submittedCount + (isFrameActive ? 1 : 0);
    pendingDeletions.push_back({lastUsingSubmission, std::move(deletion)});
}

void VulkanRenderDevice::RunPendingDeletions()
{
    auto firstKept =
        std::stable_partition(pendingDeletions.begin(), pendingDeletions.end(), [this](const PendingDeletion& pending)
                              { return pending.LastUsingSubmission <= completedSubmissions; });
    for (auto it = pendingDeletions.begin(); it != firstKept; ++it)
    {
        it->Delete();
    }
    pendingDeletions.erase(pendingDeletions.begin(), firstKept);
}

void VulkanRenderDevice::MarkAllSubmissionsComplete()
{
    completedSubmissions = submittedCount;
    RunPendingDeletions();
}

void VulkanRenderDevice::DestroyAllResources()
{
    for (PendingDeletion& pending : pendingDeletions)
    {
        pending.Delete();
    }
    pendingDeletions.clear();

    pipelines.ForEach(
        [this](PipelineData& data)
        {
            vkDestroyPipeline(device, data.Pipeline, nullptr);
            vkDestroyPipelineLayout(device, data.Layout, nullptr);
        });
    pipelines.Clear();

    buffers.ForEach([this](BufferData& data) { vmaDestroyBuffer(allocator, data.Buffer, data.Allocation); });
    buffers.Clear();

    textures.ForEach(
        [this](TextureData& data)
        {
            vkDestroyImageView(device, data.View, nullptr);
            vmaDestroyImage(allocator, data.Image, data.Allocation);
        });
    textures.Clear();

    samplers.ForEach([this](SamplerData& data) { vkDestroySampler(device, data.Sampler, nullptr); });
    samplers.Clear();

    for (auto& [key, layout] : setLayouts)
    {
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
    setLayouts.clear();
}

} // namespace hyoshi::rhi
