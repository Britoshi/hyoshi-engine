add_library(hyoshi_rhi STATIC
    vulkan/ThirdPartyImplementations.cpp
    vulkan/VulkanCommon.cpp
    vulkan/VulkanPipelineCache.cpp
    vulkan/VulkanRenderDevice.cpp
    vulkan/VulkanResources.cpp
    vulkan/VulkanSwapchain.cpp
)
add_library(hyoshi::rhi ALIAS hyoshi_rhi)

target_include_directories(hyoshi_rhi PUBLIC include PRIVATE vulkan)
target_link_libraries(hyoshi_rhi
    PUBLIC hyoshi::core
    PRIVATE hyoshi::platform volk hyoshi_vma hyoshi_stb
)

# macOS development builds: Homebrew's loader doesn't search its own layer directory, and the
# layer manifest names its library by bare filename, which dlopen can't find in /opt/homebrew/lib.
# Write a copy of the manifest with an absolute library path and point the loader at the copy.
if(APPLE)
    set(HYOSHI_LAYER_HINTS $ENV{VULKAN_SDK} /opt/homebrew /usr/local)
    find_file(HYOSHI_VULKAN_LAYER_MANIFEST VkLayer_khronos_validation.json
        HINTS ${HYOSHI_LAYER_HINTS} PATH_SUFFIXES share/vulkan/explicit_layer.d NO_DEFAULT_PATH)
    find_library(HYOSHI_VULKAN_LAYER_LIBRARY VkLayer_khronos_validation
        HINTS ${HYOSHI_LAYER_HINTS} PATH_SUFFIXES lib NO_DEFAULT_PATH)

    if(HYOSHI_VULKAN_LAYER_MANIFEST AND HYOSHI_VULKAN_LAYER_LIBRARY)
        file(READ ${HYOSHI_VULKAN_LAYER_MANIFEST} manifest)
        string(REGEX REPLACE "\"library_path\": *\"[^\"]*\"" "\"library_path\": \"${HYOSHI_VULKAN_LAYER_LIBRARY}\""
               manifest "${manifest}")
        set(layerDir ${CMAKE_BINARY_DIR}/vulkan/explicit_layer.d)
        file(WRITE ${layerDir}/VkLayer_khronos_validation.json "${manifest}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HYOSHI_VULKAN_LAYER_MANIFEST})
        target_compile_definitions(hyoshi_rhi PRIVATE HYOSHI_VULKAN_LAYER_PATH="${layerDir}")
    endif()
endif()

hyoshi_set_warnings(hyoshi_rhi)
