add_subdirectory(core)
add_subdirectory(input)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(renderer)
add_subdirectory(debug)
add_subdirectory(audio)
add_subdirectory(rhythm)
add_subdirectory(ui)
add_subdirectory(songs)
add_subdirectory(app)
# Desktop only, like the editors built on it.
if(NOT ANDROID)
    add_subdirectory(editor)
endif()

# Everything the engine provides, for games: link this and write a hyoshi::app::Game.
add_library(hyoshi_engine INTERFACE)
add_library(hyoshi::engine ALIAS hyoshi_engine)
target_link_libraries(hyoshi_engine INTERFACE hyoshi::core hyoshi::platform hyoshi::rhi hyoshi::renderer hyoshi::debug
    hyoshi::audio hyoshi::rhythm hyoshi::input hyoshi::ui hyoshi::songs hyoshi::app hyoshi::mania hyoshi::osu_import)
