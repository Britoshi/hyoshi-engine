#include "VulkanRenderDevice.h"

#include "core/Log.h"
#include "platform/Platform.h"

// After volk (via VulkanRenderDevice.h), so SDL uses the real Vulkan types.
#include <SDL3/SDL_vulkan.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace hyoshi::rhi
{

namespace
{

constexpr uint64_t NO_TIMEOUT = UINT64_MAX;
constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
constexpr VkShaderStageFlags PUSH_CONSTANT_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension)
                       { return std::strcmp(extension.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> GetInstanceExtensions()
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return extensions;
}

std::vector<VkExtensionProperties> GetDeviceExtensions(VkPhysicalDevice physicalDevice)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
    return extensions;
}

bool HasInstanceLayer(const char* name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [name](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

SurfaceTransform ToSurfaceTransform(VkSurfaceTransformFlagBitsKHR transform)
{
    switch (transform)
    {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        return SurfaceTransform::Rotate90;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        return SurfaceTransform::Rotate180;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        return SurfaceTransform::Rotate270;
    default:
        return SurfaceTransform::Identity;
    }
}

VkPrimitiveTopology ToVkTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::TriangleList:
        break;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat ToVkFormat(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float:
        return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::UByte4Norm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::UInt:
        return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

const char* DeviceTypeName(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

} // namespace

std::unique_ptr<IRenderDevice> CreateRenderDevice()
{
    return std::make_unique<VulkanRenderDevice>();
}

// --- VulkanCommandList ---

void VulkanCommandList::Reset(VkCommandBuffer newCommandBuffer)
{
    commandBuffer = newCommandBuffer;
    boundLayout = VK_NULL_HANDLE;
    boundSetLayout = VK_NULL_HANDLE;
    boundTextureCount = 0;
    boundTextures = {};
    areTexturesDirty = false;
    isInRenderPass = false;
}

void VulkanCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    HYOSHI_ASSERT(!isInRenderPass, "Render passes can't nest");

    const VkExtent2D extent = device.swapchain.GetExtent();

    VkClearValue clearValue{};
    clearValue.color = {{desc.Clear.R, desc.Clear.G, desc.Clear.B, desc.Clear.A}};

    VkRenderPassBeginInfo beginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    beginInfo.renderPass = device.swapchainRenderPass;
    beginInfo.framebuffer = device.swapchain.GetFramebuffer(device.imageIndex);
    beginInfo.renderArea = {{0, 0}, extent};
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    isInRenderPass = true;

    SetViewport({0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
    SetScissor({0, 0, extent.width, extent.height});
}

void VulkanCommandList::EndRenderPass()
{
    HYOSHI_ASSERT(isInRenderPass);
    vkCmdEndRenderPass(commandBuffer);
    isInRenderPass = false;
}

void VulkanCommandList::BindPipeline(PipelineHandle pipeline)
{
    const VulkanRenderDevice::PipelineData* data = device.pipelines.Get(pipeline);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed pipeline");
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, data->Pipeline);
    boundLayout = data->Layout;
    boundSetLayout = data->SetLayout;
    boundTextureCount = data->TextureCount;
    areTexturesDirty = boundTextureCount > 0;
}

void VulkanCommandList::BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset)
{
    const VulkanRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    const VkDeviceSize vkOffset = offset;
    vkCmdBindVertexBuffers(commandBuffer, binding, 1, &data->Buffer, &vkOffset);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type)
{
    const VulkanRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    vkCmdBindIndexBuffer(commandBuffer, data->Buffer, offset,
                         type == IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler)
{
    HYOSHI_ASSERT(slot < MAX_TEXTURE_SLOTS);
    const VulkanRenderDevice::TextureData* textureData = device.textures.Get(texture);
    const VulkanRenderDevice::SamplerData* samplerData = device.samplers.Get(sampler);
    HYOSHI_ASSERT(textureData != nullptr && samplerData != nullptr, "Invalid or destroyed texture or sampler");

    boundTextures[slot] = {samplerData->Sampler, textureData->View, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    areTexturesDirty = true;
}

void VulkanCommandList::FlushTextures()
{
    if (!areTexturesDirty || boundTextureCount == 0)
    {
        return;
    }
    areTexturesDirty = false;

    const VkDescriptorSet set = device.AllocateDescriptorSet(boundSetLayout);
    if (set == VK_NULL_HANDLE)
    {
        return;
    }

    std::array<VkWriteDescriptorSet, MAX_TEXTURE_SLOTS> writes{};
    for (uint32_t slot = 0; slot < boundTextureCount; ++slot)
    {
        HYOSHI_ASSERT(boundTextures[slot].imageView != VK_NULL_HANDLE, "A texture slot the pipeline uses is unbound");
        writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[slot].dstSet = set;
        writes[slot].dstBinding = slot;
        writes[slot].descriptorCount = 1;
        writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[slot].pImageInfo = &boundTextures[slot];
    }
    vkUpdateDescriptorSets(device.device, boundTextureCount, writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, boundLayout, 0, 1, &set, 0, nullptr);
}

void VulkanCommandList::PushConstants(const void* data, uint32_t size)
{
    HYOSHI_ASSERT(boundLayout != VK_NULL_HANDLE, "Bind a pipeline before pushing constants");
    vkCmdPushConstants(commandBuffer, boundLayout, PUSH_CONSTANT_STAGES, 0, size, data);
}

void VulkanCommandList::SetViewport(const Viewport& viewport)
{
    const VkViewport vkViewport{viewport.X, viewport.Y, viewport.Width, viewport.Height, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &vkViewport);
}

void VulkanCommandList::SetScissor(const Rect& scissor)
{
    const VkRect2D vkScissor{{scissor.X, scissor.Y}, {scissor.Width, scissor.Height}};
    vkCmdSetScissor(commandBuffer, 0, 1, &vkScissor);
}

void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    FlushTextures();
    vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t firstInstance)
{
    FlushTextures();
    vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// --- VulkanRenderDevice: setup and teardown ---

VulkanRenderDevice::VulkanRenderDevice() : commandList(*this)
{
}

VulkanRenderDevice::~VulkanRenderDevice()
{
    Shutdown();
}

Result<void> VulkanRenderDevice::Initialize(const DeviceConfig& config, platform::Platform& targetPlatform)
{
    HYOSHI_ASSERT(!isInitialized);
    platform = &targetPlatform;

    // Share the Vulkan library SDL loaded, rather than volk loading a second copy.
    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (getInstanceProcAddr == nullptr)
    {
        return Error{"The Vulkan library isn't loaded; initialize the platform with EnableVulkan"};
    }
    volkInitializeCustom(getInstanceProcAddr);

    Result<void> result = CreateInstance(config);
    if (result && !SDL_Vulkan_CreateSurface(platform->GetNativeWindow(), instance, nullptr, &surface))
    {
        result = Error{std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError()};
    }
    if (result)
    {
        result = SelectPhysicalDevice();
    }
    if (result)
    {
        result = CreateLogicalDevice();
    }
    if (result)
    {
        result = CreateAllocator();
    }
    if (result)
    {
        result = pipelineCache.Create(device, physicalDeviceProperties, config.PipelineCachePath);
    }
    if (result)
    {
        result = CreateFrameResources();
    }
    if (result)
    {
        result = CreateSwapchainResources();
    }

    if (!result)
    {
        Shutdown();
        return result;
    }

    isInitialized = true;
    return {};
}

void VulkanRenderDevice::Shutdown()
{
    if (instance == VK_NULL_HANDLE)
    {
        return;
    }

    if (device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device);
        pipelineCache.Save();
        DestroyAllResources();

        if (screenshotBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator, screenshotBuffer, screenshotAllocation);
            screenshotBuffer = VK_NULL_HANDLE;
        }

        for (FrameData& frame : frames)
        {
            if (frame.CommandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(device, frame.CommandPool, nullptr);
            }
            if (frame.ImageAvailable != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, frame.ImageAvailable, nullptr);
            }
            if (frame.InFlight != VK_NULL_HANDLE)
            {
                vkDestroyFence(device, frame.InFlight, nullptr);
            }
            for (VkDescriptorPool pool : frame.DescriptorPools)
            {
                vkDestroyDescriptorPool(device, pool, nullptr);
            }
            frame = {};
        }

        if (immediatePool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, immediatePool, nullptr);
            immediatePool = VK_NULL_HANDLE;
            immediateCommandBuffer = VK_NULL_HANDLE;
        }
        if (immediateFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, immediateFence, nullptr);
            immediateFence = VK_NULL_HANDLE;
        }

        swapchain.Destroy();
        if (swapchainRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(device, swapchainRenderPass, nullptr);
            swapchainRenderPass = VK_NULL_HANDLE;
            renderPassFormat = VK_FORMAT_UNDEFINED;
        }

        pipelineCache.Destroy();
        if (allocator != VK_NULL_HANDLE)
        {
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }

        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    if (surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    if (debugMessenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }

    vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    isInitialized = false;
}

Result<void> VulkanRenderDevice::CreateInstance(const DeviceConfig& config)
{
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        vkEnumerateInstanceVersion(&loaderVersion);
    }
    if (loaderVersion < VK_API_VERSION_1_1)
    {
        return Error{"Vulkan 1.1 is required"};
    }

    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr)
    {
        return Error{std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError()};
    }
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

    const std::vector<VkExtensionProperties> available = GetInstanceExtensions();

    // MoltenVK is a "portability" implementation, only listed when the app opts in.
    VkInstanceCreateFlags flags = 0;
    if (HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    std::vector<const char*> layers;
    bool enableDebugMessenger = false;
    if (config.EnableValidation)
    {
#if defined(HYOSHI_VULKAN_LAYER_PATH)
        // Development: Homebrew's loader doesn't search its own layer directory. The loader reads
        // this when it first enumerates layers, which is still ahead of us.
        setenv("VK_ADD_LAYER_PATH", HYOSHI_VULKAN_LAYER_PATH, 0);
#endif
        if (HasInstanceLayer(VALIDATION_LAYER))
        {
            layers.push_back(VALIDATION_LAYER);
        }
        else
        {
            HYOSHI_LOG_WARN("Validation requested, but {} is not installed", VALIDATION_LAYER);
        }

        // Only with the layer. Without it, every message is the loader's own (on Windows without
        // the Vulkan SDK, an empty layer registry), and would count as a validation failure.
        if (!layers.empty() && HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            enableDebugMessenger = true;
        }
    }

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = config.ApplicationName.c_str();
    applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.pEngineName = "Hyoshi";
    applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = OnDebugMessage;
    messengerInfo.pUserData = this;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    // Chaining the messenger also reports problems in vkCreateInstance and vkDestroyInstance.
    createInfo.pNext = enableDebugMessenger ? &messengerInfo : nullptr;
    createInfo.flags = flags;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (Result<void> result = CheckVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance"); !result)
    {
        return result;
    }
    volkLoadInstance(instance);

    if (enableDebugMessenger)
    {
        if (Result<void> result =
                CheckVk(vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &debugMessenger),
                        "vkCreateDebugUtilsMessengerEXT");
            !result)
        {
            return result;
        }
    }

    HYOSHI_LOG_INFO("Vulkan instance: loader {}.{}.{}, validation {}", VK_API_VERSION_MAJOR(loaderVersion),
                    VK_API_VERSION_MINOR(loaderVersion), VK_API_VERSION_PATCH(loaderVersion),
                    layers.empty() ? "off" : "on");
    return {};
}

Result<void> VulkanRenderDevice::SelectPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> candidates(count);
    vkEnumeratePhysicalDevices(instance, &count, candidates.data());

    int bestScore = -1;
    for (VkPhysicalDevice candidate : candidates)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1 ||
            !HasExtension(GetDeviceExtensions(candidate), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            continue;
        }

        // One queue family that can both render and present to our surface.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        for (uint32_t family = 0; family < familyCount; ++family)
        {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &canPresent);
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || canPresent == VK_FALSE)
            {
                continue;
            }

            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                              : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                                : 0;
            if (score > bestScore)
            {
                bestScore = score;
                physicalDevice = candidate;
                physicalDeviceProperties = properties;
                queueFamily = family;
            }
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE)
    {
        return Error{"No GPU supports Vulkan 1.1 with presentation to this window"};
    }

    const uint32_t apiVersion = physicalDeviceProperties.apiVersion;
    HYOSHI_LOG_INFO("GPU: {} ({}, Vulkan {}.{}.{})", physicalDeviceProperties.deviceName,
                    DeviceTypeName(physicalDeviceProperties.deviceType), VK_API_VERSION_MAJOR(apiVersion),
                    VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));
    return {};
}

Result<void> VulkanRenderDevice::CreateLogicalDevice()
{
    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Required whenever the implementation (MoltenVK) exposes it.
    constexpr const char* PORTABILITY_SUBSET = "VK_KHR_portability_subset";
    hasPortabilitySubset = HasExtension(GetDeviceExtensions(physicalDevice), PORTABILITY_SUBSET);
    if (hasPortabilitySubset)
    {
        extensions.push_back(PORTABILITY_SUBSET);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (Result<void> result = CheckVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
        !result)
    {
        return result;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    return {};
}

Result<void> VulkanRenderDevice::CreateAllocator()
{
    VmaAllocatorCreateInfo createInfo{};
    createInfo.vulkanApiVersion = VK_API_VERSION_1_1;
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;
    createInfo.instance = instance;

    VmaVulkanFunctions functions{};
    if (Result<void> result =
            CheckVk(vmaImportVulkanFunctionsFromVolk(&createInfo, &functions), "vmaImportVulkanFunctionsFromVolk");
        !result)
    {
        return result;
    }
    createInfo.pVulkanFunctions = &functions;

    return CheckVk(vmaCreateAllocator(&createInfo, &allocator), "vmaCreateAllocator");
}

Result<void> VulkanRenderDevice::CreateFrameResources()
{
    for (FrameData& frame : frames)
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        if (Result<void> result =
                CheckVk(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.CommandPool), "vkCreateCommandPool");
            !result)
        {
            return result;
        }

        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = frame.CommandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        if (Result<void> result = CheckVk(vkAllocateCommandBuffers(device, &allocateInfo, &frame.CommandBuffer),
                                          "vkAllocateCommandBuffers");
            !result)
        {
            return result;
        }

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (Result<void> result =
                CheckVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.ImageAvailable), "vkCreateSemaphore");
            !result)
        {
            return result;
        }

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (Result<void> result = CheckVk(vkCreateFence(device, &fenceInfo, nullptr, &frame.InFlight), "vkCreateFence");
            !result)
        {
            return result;
        }
    }

    // One-off uploads outside the frame (textures, static buffers).
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (Result<void> result =
            CheckVk(vkCreateCommandPool(device, &poolInfo, nullptr, &immediatePool), "vkCreateCommandPool");
        !result)
    {
        return result;
    }

    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = immediatePool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (Result<void> result = CheckVk(vkAllocateCommandBuffers(device, &allocateInfo, &immediateCommandBuffer),
                                      "vkAllocateCommandBuffers");
        !result)
    {
        return result;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return CheckVk(vkCreateFence(device, &fenceInfo, nullptr, &immediateFence), "vkCreateFence");
}

// --- Surface and swapchain ---

Result<void> VulkanRenderDevice::CreateSwapchainResources()
{
    const platform::PixelSize pixelSize = platform->GetWindowPixelSize();

    SwapchainCreateInfo createInfo;
    createInfo.PhysicalDevice = physicalDevice;
    createInfo.Device = device;
    createInfo.Surface = surface;
    createInfo.WindowExtent = {static_cast<uint32_t>(std::max(pixelSize.Width, 0)),
                               static_cast<uint32_t>(std::max(pixelSize.Height, 0))};

    Result<bool> created = swapchain.Create(createInfo);
    if (!created)
    {
        return created.GetError();
    }
    if (!created.Value())
    {
        // Zero-sized window; BeginFrame retries.
        isSwapchainDirty = true;
        return {};
    }

    if (swapchain.GetFormat() != renderPassFormat)
    {
        if (swapchainRenderPass != VK_NULL_HANDLE && !pipelines.IsEmpty())
        {
            HYOSHI_LOG_WARN("Swapchain format changed; existing pipelines target the old render pass");
        }
        if (Result<void> result = CreateSwapchainRenderPass(swapchain.GetFormat()); !result)
        {
            return result;
        }
    }

    if (Result<void> result = swapchain.CreateFramebuffers(swapchainRenderPass); !result)
    {
        return result;
    }

    isSwapchainDirty = false;
    return {};
}

Result<void> VulkanRenderDevice::CreateSwapchainRenderPass(VkFormat format)
{
    if (swapchainRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, swapchainRenderPass, nullptr);
        swapchainRenderPass = VK_NULL_HANDLE;
    }

    // Explicit clear-on-load and store-on-exit: exactly what tile-based GPUs want to know.
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;

    // Wait for the acquired image (the submit waits on the acquire semaphore at this stage).
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &attachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    if (Result<void> result =
            CheckVk(vkCreateRenderPass(device, &createInfo, nullptr, &swapchainRenderPass), "vkCreateRenderPass");
        !result)
    {
        return result;
    }
    renderPassFormat = format;
    return {};
}

Result<void> VulkanRenderDevice::CreateSurface()
{
    if (surface != VK_NULL_HANDLE)
    {
        return {};
    }

    if (!SDL_Vulkan_CreateSurface(platform->GetNativeWindow(), instance, nullptr, &surface))
    {
        return Error{std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError()};
    }

    VkBool32 canPresent = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamily, surface, &canPresent);
    if (canPresent == VK_FALSE)
    {
        return Error{"The new surface can't be presented from the render queue"};
    }

    return CreateSwapchainResources();
}

void VulkanRenderDevice::DestroySurface()
{
    if (surface == VK_NULL_HANDLE)
    {
        return;
    }

    vkDeviceWaitIdle(device);
    MarkAllSubmissionsComplete();
    pipelineCache.Save();
    swapchain.Destroy();
    vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
}

bool VulkanRenderDevice::HasSurface() const
{
    return surface != VK_NULL_HANDLE;
}

Result<void> VulkanRenderDevice::RecreateSwapchain()
{
    if (surface == VK_NULL_HANDLE)
    {
        return {};
    }

    vkDeviceWaitIdle(device);
    MarkAllSubmissionsComplete();
    return CreateSwapchainResources();
}

// --- Pipelines ---

Result<PipelineHandle> VulkanRenderDevice::CreatePipeline(const PipelineDesc& desc)
{
    HYOSHI_ASSERT(swapchainRenderPass != VK_NULL_HANDLE, "Pipelines need the swapchain render pass");

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = desc.Spirv.size_bytes();
    moduleInfo.pCode = desc.Spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (Result<void> result =
            CheckVk(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        !result)
    {
        return result.GetError();
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = module;
    stages[0].pName = desc.VertexEntry;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = module;
    stages[1].pName = desc.FragmentEntry;

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(desc.VertexBindings.size());
    for (const VertexBindingDesc& binding : desc.VertexBindings)
    {
        bindings.push_back({binding.Binding, binding.Stride,
                            binding.InputRate == VertexInputRate::Instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                           : VK_VERTEX_INPUT_RATE_VERTEX});
    }

    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.VertexAttributes.size());
    for (const VertexAttributeDesc& attribute : desc.VertexAttributes)
    {
        attributes.push_back({attribute.Location, attribute.Binding, ToVkFormat(attribute.Format), attribute.Offset});
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = ToVkTopology(desc.Topology);
    // Metal always restarts strips at the maximum index and MoltenVK reports disabling it as an
    // error, so strips enable it everywhere. Only indexed draws can hit the restart index.
    inputAssembly.primitiveRestartEnable = desc.Topology == PrimitiveTopology::TriangleStrip ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.Blend == BlendMode::Alpha)
    {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    constexpr std::array<VkDynamicState, 2> DYNAMIC_STATES = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(DYNAMIC_STATES.size());
    dynamicState.pDynamicStates = DYNAMIC_STATES.data();

    PipelineData data;
    data.TextureCount = desc.TextureCount;

    Result<void> result;
    if (desc.TextureCount > 0)
    {
        Result<VkDescriptorSetLayout> setLayout = GetSetLayout(desc.TextureCount);
        if (setLayout)
        {
            data.SetLayout = setLayout.Value();
        }
        else
        {
            result = setLayout.GetError();
        }
    }

    const VkPushConstantRange pushConstantRange{PUSH_CONSTANT_STAGES, 0, desc.PushConstantSize};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = data.SetLayout != VK_NULL_HANDLE ? 1 : 0;
    layoutInfo.pSetLayouts = &data.SetLayout;
    layoutInfo.pushConstantRangeCount = desc.PushConstantSize > 0 ? 1 : 0;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (result)
    {
        result = CheckVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &data.Layout), "vkCreatePipelineLayout");
    }

    if (result)
    {
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = data.Layout;
        pipelineInfo.renderPass = swapchainRenderPass;
        pipelineInfo.subpass = 0;

        result =
            CheckVk(vkCreateGraphicsPipelines(device, pipelineCache.Get(), 1, &pipelineInfo, nullptr, &data.Pipeline),
                    "vkCreateGraphicsPipelines");
    }

    vkDestroyShaderModule(device, module, nullptr);

    if (!result)
    {
        if (data.Layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device, data.Layout, nullptr);
        }
        return Error{result.GetError().Message + " (" + desc.DebugName + ")"};
    }

    return pipelines.Create(data);
}

void VulkanRenderDevice::Destroy(PipelineHandle pipeline)
{
    const PipelineData* data = pipelines.Get(pipeline);
    if (data == nullptr)
    {
        return;
    }

    DeferDeletion(
        [vkDevice = device, vkPipeline = data->Pipeline, layout = data->Layout]
        {
            vkDestroyPipeline(vkDevice, vkPipeline, nullptr);
            vkDestroyPipelineLayout(vkDevice, layout, nullptr);
        });
    pipelines.Destroy(pipeline);
}

// --- Frames ---

FrameStatus VulkanRenderDevice::BeginFrame()
{
    HYOSHI_ASSERT(!isFrameActive, "BeginFrame called twice");

    if (surface == VK_NULL_HANDLE)
    {
        return FrameStatus::Skipped;
    }

    if (isSwapchainDirty || !swapchain.IsValid())
    {
        if (Result<void> result = RecreateSwapchain(); !result)
        {
            HYOSHI_LOG_ERROR("Swapchain recreation failed: {}", result.GetError().Message);
            return FrameStatus::Skipped;
        }
        if (isSwapchainDirty || !swapchain.IsValid())
        {
            return FrameStatus::Skipped;
        }
    }

    FrameData& frame = frames[frameIndex];
    vkWaitForFences(device, 1, &frame.InFlight, VK_TRUE, NO_TIMEOUT);

    // One queue completes submissions in order, so everything up to this slot's last one is done.
    completedSubmissions = std::max(completedSubmissions, frame.SubmissionNumber);
    RunPendingDeletions();

    const VkResult acquireResult =
        vkAcquireNextImageKHR(device, swapchain.Get(), NO_TIMEOUT, frame.ImageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        isSwapchainDirty = true;
        return FrameStatus::Skipped;
    }
    if (acquireResult == VK_SUBOPTIMAL_KHR)
    {
        // Still usable for this frame; recreate before the next one.
        isSwapchainDirty = true;
    }
    else if (acquireResult != VK_SUCCESS)
    {
        HYOSHI_LOG_ERROR("vkAcquireNextImageKHR failed: {}", ToString(acquireResult));
        return FrameStatus::Skipped;
    }

    // Only reset once an image is certain, so a skipped frame never leaves the fence unsignaled.
    vkResetFences(device, 1, &frame.InFlight);
    vkResetCommandPool(device, frame.CommandPool, 0);
    for (VkDescriptorPool pool : frame.DescriptorPools)
    {
        vkResetDescriptorPool(device, pool, 0);
    }
    frame.ActiveDescriptorPool = 0;

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.CommandBuffer, &beginInfo);

    commandList.Reset(frame.CommandBuffer);
    isFrameActive = true;
    return FrameStatus::Ready;
}

ICommandList& VulkanRenderDevice::GetCommandList()
{
    HYOSHI_ASSERT(isFrameActive, "GetCommandList is only valid between BeginFrame and EndFrameAndPresent");
    return commandList;
}

void VulkanRenderDevice::EndFrameAndPresent()
{
    HYOSHI_ASSERT(isFrameActive);
    FrameData& frame = frames[frameIndex];

    if (isScreenshotRequested)
    {
        if (swapchain.SupportsTransferSource())
        {
            RecordScreenshotCopy(frame.CommandBuffer, swapchain.GetImage(imageIndex));
        }
        else
        {
            HYOSHI_LOG_WARN("Screenshot skipped: swapchain images can't be copied on this device");
        }
        isScreenshotRequested = false;
    }

    vkEndCommandBuffer(frame.CommandBuffer);

    const VkSemaphore renderFinished = swapchain.GetRenderFinishedSemaphore(imageIndex);
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.ImageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished;

    const VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, frame.InFlight);
    if (submitResult != VK_SUCCESS)
    {
        HYOSHI_LOG_CRITICAL("vkQueueSubmit failed: {}", ToString(submitResult));
    }
    frame.SubmissionNumber = ++submittedCount;

    const VkSwapchainKHR swapchainHandle = swapchain.Get();
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainHandle;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(queue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        isSwapchainDirty = true;
    }
    else if (presentResult != VK_SUCCESS)
    {
        HYOSHI_LOG_ERROR("vkQueuePresentKHR failed: {}", ToString(presentResult));
    }

    isFrameActive = false;

    if (isScreenshotRecorded)
    {
        vkQueueWaitIdle(queue);
        MarkAllSubmissionsComplete();
        WritePendingScreenshot();
    }

    frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
}

uint32_t VulkanRenderDevice::GetFrameIndex() const
{
    return frameIndex;
}

Extent VulkanRenderDevice::GetSwapchainExtent() const
{
    const VkExtent2D extent = swapchain.GetExtent();
    return {extent.width, extent.height};
}

SurfaceTransform VulkanRenderDevice::GetSurfaceTransform() const
{
    return ToSurfaceTransform(swapchain.GetTransform());
}

void VulkanRenderDevice::SavePipelineCache()
{
    pipelineCache.Save();
}

// --- Debugging ---

VkBool32 VKAPI_PTR VulkanRenderDevice::OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
{
    auto* self = static_cast<VulkanRenderDevice*>(userData);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        HYOSHI_LOG_ERROR("Vulkan: {}", data->pMessage);
        ++self->validationMessageCount;
    }
    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
    {
        HYOSHI_LOG_WARN("Vulkan: {}", data->pMessage);
        ++self->validationMessageCount;
    }
    return VK_FALSE;
}

uint32_t VulkanRenderDevice::GetValidationMessageCount() const
{
    return validationMessageCount;
}

void VulkanRenderDevice::RequestScreenshot(std::string path)
{
    screenshotPath = std::move(path);
    isScreenshotRequested = true;
}

void VulkanRenderDevice::RecordScreenshotCopy(VkCommandBuffer commandBuffer, VkImage image)
{
    screenshotExtent = swapchain.GetExtent();
    screenshotFormat = swapchain.GetFormat();

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = VkDeviceSize{screenshotExtent.width} * screenshotExtent.height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &screenshotBuffer, &screenshotAllocation, nullptr) !=
        VK_SUCCESS)
    {
        HYOSHI_LOG_WARN("Screenshot skipped: could not allocate the readback buffer");
        return;
    }

    // The render pass left the image ready to present; borrow it for a copy, then hand it back.
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {screenshotExtent.width, screenshotExtent.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer, 1, &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = screenshotBuffer;
    toHost.size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost,
                         1, &toPresent);

    isScreenshotRecorded = true;
}

void VulkanRenderDevice::WritePendingScreenshot()
{
    isScreenshotRecorded = false;

    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator, screenshotAllocation, &allocationInfo);
    vmaInvalidateAllocation(allocator, screenshotAllocation, 0, VK_WHOLE_SIZE);

    const auto* source = static_cast<const uint8_t*>(allocationInfo.pMappedData);
    const size_t pixelCount = size_t{screenshotExtent.width} * screenshotExtent.height;
    const bool isBgra = screenshotFormat == VK_FORMAT_B8G8R8A8_UNORM || screenshotFormat == VK_FORMAT_B8G8R8A8_SRGB;

    std::vector<uint8_t> rgba(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint8_t* pixel = source + i * 4;
        rgba[i * 4 + 0] = isBgra ? pixel[2] : pixel[0];
        rgba[i * 4 + 1] = pixel[1];
        rgba[i * 4 + 2] = isBgra ? pixel[0] : pixel[2];
        rgba[i * 4 + 3] = 255;
    }

    const int width = static_cast<int>(screenshotExtent.width);
    const int height = static_cast<int>(screenshotExtent.height);
    if (stbi_write_png(screenshotPath.c_str(), width, height, 4, rgba.data(), width * 4) != 0)
    {
        HYOSHI_LOG_INFO("Screenshot saved to {}", screenshotPath);
    }
    else
    {
        HYOSHI_LOG_WARN("Could not write screenshot to {}", screenshotPath);
    }

    vmaDestroyBuffer(allocator, screenshotBuffer, screenshotAllocation);
    screenshotBuffer = VK_NULL_HANDLE;
    screenshotAllocation = VK_NULL_HANDLE;
}

} // namespace hyoshi::rhi
