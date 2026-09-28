#pragma once

#include "VulkanCommon.h"

#include <string>

namespace hyoshi::rhi
{

// A VkPipelineCache persisted to disk so pipelines don't recompile (and hitch) on every launch.
class VulkanPipelineCache
{
public:
    // Seeds the cache from `path` when that file was written by this exact device and driver.
    // An empty path gives an in-memory cache only.
    Result<void> Create(VkDevice device, const VkPhysicalDeviceProperties& properties, std::string path);
    void Save() const;
    void Destroy();

    VkPipelineCache Get() const
    {
        return cache;
    }

private:
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::string path;
};

} // namespace hyoshi::rhi
