#include "VulkanSwapchain.h"

#include "core/Log.h"

#include <algorithm>
#include <array>
#include <utility>

namespace hyoshi::rhi
{

namespace
{

// Colors are authored in sRGB and written as-is, so prefer UNORM formats. Android devices commonly
// expose R8G8B8A8, Apple and desktop B8G8R8A8.
VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available)
{
    constexpr std::array<VkFormat, 2> PREFERRED = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat preferred : PREFERRED)
    {
        for (const VkSurfaceFormatKHR& candidate : available)
        {
            if (candidate.format == preferred && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                return candidate;
            }
        }
    }
    return available.front();
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    // Many Android surfaces only offer INHERIT.
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> PREFERRED = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (VkCompositeAlphaFlagBitsKHR preferred : PREFERRED)
    {
        if ((supported & preferred) != 0)
        {
            return preferred;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

bool IsRotated90Or270(VkSurfaceTransformFlagBitsKHR transform)
{
    return transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
}

} // namespace

Result<bool> VulkanSwapchain::Create(const SwapchainCreateInfo& info)
{
    device = info.Device;

    VkSurfaceCapabilitiesKHR capabilities{};
    if (Result<void> result =
            CheckVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(info.PhysicalDevice, info.Surface, &capabilities),
                    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        !result)
    {
        return result.GetError();
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(info.PhysicalDevice, info.Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(info.PhysicalDevice, info.Surface, &formatCount, formats.data());
    if (formats.empty())
    {
        return Error{"The surface reports no formats"};
    }

    // Pre-rotation (DESIGN.md section 10.2): render in the display's native orientation so the
    // Android compositor doesn't have to rotate every frame. The game applies the matching rotation
    // to its projection; see GetTransform().
    transform = capabilities.currentTransform;

    VkExtent2D newExtent = capabilities.currentExtent;
    if (newExtent.width == UINT32_MAX)
    {
        newExtent.width =
            std::clamp(info.WindowExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        newExtent.height = std::clamp(info.WindowExtent.height, capabilities.minImageExtent.height,
                                      capabilities.maxImageExtent.height);
    }
    if (IsRotated90Or270(transform))
    {
        std::swap(newExtent.width, newExtent.height);
    }
    if (newExtent.width == 0 || newExtent.height == 0)
    {
        return false;
    }

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    supportsTransferSource = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (supportsTransferSource)
    {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    const VkSwapchainKHR oldSwapchain = swapchain;

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = info.Surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = newExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = usage;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = transform;
    createInfo.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    // FIFO is the only mode every device supports, and it paces frames to the display.
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (Result<void> result =
            CheckVk(vkCreateSwapchainKHR(device, &createInfo, nullptr, &newSwapchain), "vkCreateSwapchainKHR");
        !result)
    {
        return result.GetError();
    }

    DestroyImageResources();
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    }

    swapchain = newSwapchain;
    format = surfaceFormat.format;
    extent = newExtent;

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, nullptr);
    images.resize(actualImageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, images.data());

    imageViews.resize(actualImageCount, VK_NULL_HANDLE);
    renderFinished.resize(actualImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < actualImageCount; ++i)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (Result<void> result =
                CheckVk(vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]), "vkCreateImageView");
            !result)
        {
            return result.GetError();
        }

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (Result<void> result =
                CheckVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished[i]), "vkCreateSemaphore");
            !result)
        {
            return result.GetError();
        }
    }

    HYOSHI_LOG_DEBUG("Swapchain {}x{}, {} images, transform {}", extent.width, extent.height, actualImageCount,
                     static_cast<uint32_t>(transform));
    return true;
}

Result<void> VulkanSwapchain::CreateFramebuffers(VkRenderPass renderPass)
{
    for (VkFramebuffer framebuffer : framebuffers)
    {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    framebuffers.assign(imageViews.size(), VK_NULL_HANDLE);

    for (size_t i = 0; i < imageViews.size(); ++i)
    {
        VkFramebufferCreateInfo createInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        createInfo.renderPass = renderPass;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &imageViews[i];
        createInfo.width = extent.width;
        createInfo.height = extent.height;
        createInfo.layers = 1;
        if (Result<void> result =
                CheckVk(vkCreateFramebuffer(device, &createInfo, nullptr, &framebuffers[i]), "vkCreateFramebuffer");
            !result)
        {
            return result;
        }
    }
    return {};
}

void VulkanSwapchain::Destroy()
{
    DestroyImageResources();
    if (swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

void VulkanSwapchain::DestroyImageResources()
{
    for (VkFramebuffer framebuffer : framebuffers)
    {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (VkImageView view : imageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    for (VkSemaphore semaphore : renderFinished)
    {
        if (semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
    framebuffers.clear();
    imageViews.clear();
    renderFinished.clear();
    images.clear();
}

} // namespace hyoshi::rhi
