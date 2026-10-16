#pragma once

#include "VulkanCommon.h"
#include "rhi/RhiTypes.h"

#include <cstdint>
#include <vector>

namespace hyoshi::rhi
{

struct SwapchainCreateInfo
{
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkSurfaceKHR Surface = VK_NULL_HANDLE;
    // Window size in pixels. Only used when the surface doesn't dictate its own extent.
    VkExtent2D WindowExtent{};
    // Used when the surface offers it (DeviceConfig::PreferredPresentMode); FIFO otherwise.
    PresentMode PreferredPresentMode = PresentMode::Fifo;
};

// Swapchain images, their views and framebuffers, and one render-finished semaphore per image.
class VulkanSwapchain
{
public:
    // Creates the swapchain, or recreates it in place if one exists. Returns false without an error
    // when the surface currently has zero area (a minimized window); try again later.
    Result<bool> Create(const SwapchainCreateInfo& info);
    // One per image, each with the image and the depth buffer's view.
    Result<void> CreateFramebuffers(VkRenderPass renderPass, VkImageView depthView);
    void Destroy();

    bool IsValid() const
    {
        return swapchain != VK_NULL_HANDLE;
    }

    VkSwapchainKHR Get() const
    {
        return swapchain;
    }

    VkFormat GetFormat() const
    {
        return format;
    }

    VkExtent2D GetExtent() const
    {
        return extent;
    }

    VkSurfaceTransformFlagBitsKHR GetTransform() const
    {
        return transform;
    }

    bool SupportsTransferSource() const
    {
        return supportsTransferSource;
    }

    uint32_t GetImageCount() const
    {
        return static_cast<uint32_t>(images.size());
    }

    VkImage GetImage(uint32_t index) const
    {
        return images[index];
    }

    VkFramebuffer GetFramebuffer(uint32_t index) const
    {
        return framebuffers[index];
    }

    VkSemaphore GetRenderFinishedSemaphore(uint32_t index) const
    {
        return renderFinished[index];
    }

private:
    void DestroyImageResources();

    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // None yet, so the first one chosen is logged.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
    bool supportsTransferSource = false;

    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished;
};

} // namespace hyoshi::rhi
