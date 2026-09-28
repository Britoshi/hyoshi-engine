#pragma once

// volk must come before any other Vulkan-related header (including SDL_vulkan.h), so every Vulkan
// call goes through the function pointers volk loads.
#include <volk.h>

#include "core/Result.h"

#include <string>

namespace hyoshi::rhi
{

const char* ToString(VkResult result);

// Turns a failed Vulkan call into an Error naming the call.
inline Result<void> CheckVk(VkResult result, const char* call)
{
    if (result == VK_SUCCESS)
    {
        return {};
    }
    return Error{std::string(call) + " failed: " + ToString(result)};
}

} // namespace hyoshi::rhi
