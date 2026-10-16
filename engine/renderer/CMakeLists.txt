add_library(hyoshi_renderer STATIC
    src/Camera2D.cpp
    src/Camera3D.cpp
    src/Image.cpp
    src/Mesh.cpp
    src/Model.cpp
    src/Renderer3D.cpp
    src/Skinning.cpp
    src/SpringBones.cpp
    src/SpriteBatch.cpp
    src/TextRenderer.cpp
    src/ThirdPartyImplementations.cpp
)
add_library(hyoshi::renderer ALIAS hyoshi_renderer)

target_include_directories(hyoshi_renderer PUBLIC include)
target_link_libraries(hyoshi_renderer PUBLIC hyoshi::core hyoshi::rhi glm::glm PRIVATE hyoshi_stb hyoshi_cgltf)

hyoshi_add_shaders(hyoshi_renderer
    ${PROJECT_SOURCE_DIR}/shaders/Mesh.slang
    ${PROJECT_SOURCE_DIR}/shaders/Sprite.slang
)

# stb's decoding and resizing are optimized in debug builds too (see ThirdPartyImplementations.cpp,
# which does it for MSVC): unoptimized, a song's picture takes up to half a second to resize. So is
# skinning, which moves tens of thousands of vertices every frame.
if(NOT MSVC)
    set_source_files_properties(src/ThirdPartyImplementations.cpp src/Skinning.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

hyoshi_set_warnings(hyoshi_renderer)
