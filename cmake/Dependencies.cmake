# Third-party dependencies, fetched with CPM and pinned to exact releases.
# Versions are tracked in docs/REFERENCES.md.

CPMAddPackage(
    NAME SDL3
    SYSTEM YES
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.16
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_EXAMPLES OFF"
        "SDL_TESTS OFF"
        "SDL_INSTALL OFF"
)

CPMAddPackage(
    NAME spdlog
    SYSTEM YES
    GITHUB_REPOSITORY gabime/spdlog
    VERSION 1.17.0
    OPTIONS
        "SPDLOG_INSTALL OFF"
        "SPDLOG_BUILD_EXAMPLE OFF"
)

if(HYOSHI_BUILD_TESTS)
    CPMAddPackage(
        NAME doctest
        SYSTEM YES
        GITHUB_REPOSITORY doctest/doctest
        VERSION 2.5.3
        OPTIONS
            "DOCTEST_WITH_TESTS OFF"
            "DOCTEST_NO_INSTALL ON"
    )
endif()

# Vulkan: headers pinned to an SDK release, loaded at runtime through volk.
CPMAddPackage(
    NAME VulkanHeaders
    SYSTEM YES
    GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
    GIT_TAG vulkan-sdk-1.4.357.0
)

CPMAddPackage(
    NAME volk
    SYSTEM YES
    GITHUB_REPOSITORY zeux/volk
    GIT_TAG 1.4.350
    OPTIONS
        "VOLK_PULL_IN_VULKAN OFF"
        "VOLK_INSTALL OFF"
)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# Header-only; the implementation is compiled in the Vulkan backend.
CPMAddPackage(
    NAME VulkanMemoryAllocator
    GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG v3.4.0
    DOWNLOAD_ONLY YES
)
add_library(hyoshi_vma INTERFACE)
target_include_directories(hyoshi_vma SYSTEM INTERFACE ${VulkanMemoryAllocator_SOURCE_DIR}/include)
target_link_libraries(hyoshi_vma INTERFACE volk)
# Vulkan functions come from volk (vmaImportVulkanFunctionsFromVolk), never linked statically.
target_compile_definitions(hyoshi_vma INTERFACE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)

# stb has no releases; pinned to a commit.
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    DOWNLOAD_ONLY YES
)
add_library(hyoshi_stb INTERFACE)
target_include_directories(hyoshi_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

CPMAddPackage(
    NAME glm
    SYSTEM YES
    GITHUB_REPOSITORY g-truc/glm
    GIT_TAG 1.0.3
    OPTIONS
        "GLM_BUILD_LIBRARY OFF"
        "GLM_BUILD_TESTS OFF"
        "GLM_BUILD_INSTALL OFF"
)

# Dear ImGui has no CMake project; build the core plus the SDL3 platform backend. Rendering goes
# through the RHI (engine/debug), not imgui_impl_vulkan, so ImGui never touches Vulkan directly.
# The docking branch, for the editor's panels (ADR 0002). Multiple viewports stay off: the RHI draws
# only the main window.
CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.9b-docking
    DOWNLOAD_ONLY YES
)
add_library(hyoshi_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)
target_include_directories(hyoshi_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(hyoshi_imgui PUBLIC SDL3::SDL3)
target_compile_definitions(hyoshi_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# Header-only; the implementation (with stb_vorbis for Ogg) is compiled in the audio backend.
# MA_NO_ENGINE and friends: the engine mixes itself (DESIGN.md section 11.2), so only the device
# and decoding layers are used.
CPMAddPackage(
    NAME miniaudio
    GITHUB_REPOSITORY mackron/miniaudio
    GIT_TAG 0.11.25
    DOWNLOAD_ONLY YES
)
add_library(hyoshi_miniaudio INTERFACE)
target_include_directories(hyoshi_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
target_link_libraries(hyoshi_miniaudio INTERFACE hyoshi_stb)
target_compile_definitions(hyoshi_miniaudio INTERFACE
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING
)

# JSON for source charts (.rchart.json) and settings.
CPMAddPackage(
    NAME yyjson
    SYSTEM YES
    GITHUB_REPOSITORY ibireme/yyjson
    GIT_TAG 0.13.0
    OPTIONS
        "YYJSON_BUILD_TESTS OFF"
        "YYJSON_BUILD_FUZZER OFF"
        "YYJSON_BUILD_MISC OFF"
        "YYJSON_BUILD_DOC OFF"
        "YYJSON_INSTALL OFF"
)

# The UI font: Noto Sans JP (SIL Open Font License 1.1), which covers Latin and Japanese. Static
# OTFs from the noto-cjk release, SHA-256 checked. The game loads them from fonts/ next to its
# executable, with the license (see game/CMakeLists.txt).
set(HYOSHI_NOTO_CJK_URL https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004)
CPMAddPackage(
    NAME NotoSansJPRegular
    URL ${HYOSHI_NOTO_CJK_URL}/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf
    URL_HASH SHA256=dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073
    DOWNLOAD_ONLY YES
    DOWNLOAD_NO_EXTRACT YES
)
CPMAddPackage(
    NAME NotoSansJPBold
    URL ${HYOSHI_NOTO_CJK_URL}/Sans/SubsetOTF/JP/NotoSansJP-Bold.otf
    URL_HASH SHA256=1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f
    DOWNLOAD_ONLY YES
    DOWNLOAD_NO_EXTRACT YES
)
CPMAddPackage(
    NAME NotoCjkLicense
    URL ${HYOSHI_NOTO_CJK_URL}/LICENSE
    URL_HASH SHA256=6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2
    DOWNLOAD_ONLY YES
    DOWNLOAD_NO_EXTRACT YES
)
# Cached so a game's build (hyoshi_add_app), outside this directory's scope, sees them too.
set(HYOSHI_FONT_FILES
    ${NotoSansJPRegular_SOURCE_DIR}/NotoSansJP-Regular.otf
    ${NotoSansJPBold_SOURCE_DIR}/NotoSansJP-Bold.otf
    CACHE INTERNAL "The engine's fonts"
)
set(HYOSHI_FONT_LICENSE ${NotoCjkLicense_SOURCE_DIR}/LICENSE CACHE INTERNAL "The fonts' license")
