#include "VulkanPipelineCache.h"

#include "core/Log.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace hyoshi::rhi
{

namespace
{

std::vector<char> ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Drivers are supposed to reject foreign cache data themselves, but some Android drivers crash on
// it instead, so check the header first.
bool IsCompatible(const std::vector<char>& data, const VkPhysicalDeviceProperties& properties)
{
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header))
    {
        return false;
    }
    std::memcpy(&header, data.data(), sizeof(header));

    return header.headerSize >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == properties.vendorID && header.deviceID == properties.deviceID &&
           std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace

Result<void> VulkanPipelineCache::Create(VkDevice vkDevice, const VkPhysicalDeviceProperties& properties,
                                         std::string cachePath)
{
    device = vkDevice;
    path = std::move(cachePath);

    std::vector<char> data;
    if (!path.empty())
    {
        data = ReadFile(path);
        if (!data.empty() && !IsCompatible(data, properties))
        {
            HYOSHI_LOG_INFO("Ignoring pipeline cache from a different device or driver");
            data.clear();
        }
    }

    VkPipelineCacheCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    createInfo.initialDataSize = data.size();
    createInfo.pInitialData = data.empty() ? nullptr : data.data();

    if (Result<void> result =
            CheckVk(vkCreatePipelineCache(device, &createInfo, nullptr, &cache), "vkCreatePipelineCache");
        !result)
    {
        return result;
    }

    HYOSHI_LOG_DEBUG("Pipeline cache: {} bytes loaded", data.size());
    return {};
}

void VulkanPipelineCache::Save() const
{
    if (cache == VK_NULL_HANDLE || path.empty())
    {
        return;
    }

    size_t size = 0;
    if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || size == 0)
    {
        return;
    }

    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS)
    {
        return;
    }

    // Write to a temporary file and rename, so a crash mid-write never leaves a torn cache.
    const std::string temporaryPath = path + ".tmp";
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(size));
        if (!file)
        {
            HYOSHI_LOG_WARN("Could not write pipeline cache to {}", temporaryPath);
            return;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error)
    {
        HYOSHI_LOG_WARN("Could not save pipeline cache to {}: {}", path, error.message());
        return;
    }

    HYOSHI_LOG_DEBUG("Pipeline cache: {} bytes saved", size);
}

void VulkanPipelineCache::Destroy()
{
    if (cache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(device, cache, nullptr);
        cache = VK_NULL_HANDLE;
    }
}

} // namespace hyoshi::rhi
