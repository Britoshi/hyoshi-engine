add_library(hyoshi_debug STATIC
    src/DebugUi.cpp
)
add_library(hyoshi::debug ALIAS hyoshi_debug)

target_include_directories(hyoshi_debug PUBLIC include)
target_link_libraries(hyoshi_debug
    PUBLIC hyoshi::core hyoshi::rhi hyoshi_imgui
    PRIVATE hyoshi::platform hyoshi::renderer
)

hyoshi_add_shaders(hyoshi_debug
    ${PROJECT_SOURCE_DIR}/shaders/DebugUi.slang
)

hyoshi_set_warnings(hyoshi_debug)
