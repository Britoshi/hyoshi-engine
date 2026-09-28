add_library(hyoshi_platform STATIC
    src/Platform.cpp
)
add_library(hyoshi::platform ALIAS hyoshi_platform)

target_include_directories(hyoshi_platform PUBLIC include)

# SDL is public because platform/EntryPoint.h includes SDL_main.h.
target_link_libraries(hyoshi_platform PUBLIC hyoshi::core hyoshi::input SDL3::SDL3)

hyoshi_set_warnings(hyoshi_platform)

# macOS development builds: Homebrew and the LunarG SDK install the Vulkan loader outside the
# default dlopen search path, so remember where it is as a fallback.
if(APPLE)
    find_library(HYOSHI_VULKAN_LOADER NAMES vulkan.1 vulkan
        HINTS $ENV{VULKAN_SDK}/lib /opt/homebrew/lib /usr/local/lib NO_DEFAULT_PATH)
    if(HYOSHI_VULKAN_LOADER)
        target_compile_definitions(hyoshi_platform PRIVATE HYOSHI_VULKAN_LOADER_PATH="${HYOSHI_VULKAN_LOADER}")
    endif()
endif()
