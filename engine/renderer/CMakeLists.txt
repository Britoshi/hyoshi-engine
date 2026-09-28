add_library(hyoshi_renderer STATIC
    src/Camera2D.cpp
    src/Image.cpp
    src/SpriteBatch.cpp
    src/TextRenderer.cpp
    src/ThirdPartyImplementations.cpp
)
add_library(hyoshi::renderer ALIAS hyoshi_renderer)

target_include_directories(hyoshi_renderer PUBLIC include)
target_link_libraries(hyoshi_renderer PUBLIC hyoshi::core hyoshi::rhi glm::glm PRIVATE hyoshi_stb)

hyoshi_add_shaders(hyoshi_renderer
    ${PROJECT_SOURCE_DIR}/shaders/Sprite.slang
)

hyoshi_set_warnings(hyoshi_renderer)
