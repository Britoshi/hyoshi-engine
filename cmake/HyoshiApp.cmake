# hyoshi_add_app(<target> OUTPUT_NAME <name> SOURCES <file>... [ASSETS <folder>...] [WINDOWS_RC <file>])
#
# A game (or sample) built on the engine: an executable on desktops, and on Android the shared
# library SDL's activity loads (libmain.so). It links every engine module (hyoshi::engine).
#
# Files the app loads at run time go next to the executable, or on Android into
# HYOSHI_ANDROID_ASSETS_DIR, where Gradle packs them as APK assets:
# - the engine's own under engine/: its fonts (engine/fonts) and logos (engine/textures);
# - each ASSETS folder under its own name: ASSETS content/textures installs as textures/.
# WINDOWS_RC adds a resource script (the executable's icon) on Windows.
function(hyoshi_add_app target)
    cmake_parse_arguments(PARSE_ARGV 1 APP "" "OUTPUT_NAME;WINDOWS_RC" "SOURCES;ASSETS")
    if(NOT APP_OUTPUT_NAME)
        set(APP_OUTPUT_NAME ${target})
    endif()

    if(ANDROID)
        add_library(${target} SHARED ${APP_SOURCES})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
        target_link_libraries(${target} PRIVATE android log)
        if(NOT HYOSHI_ANDROID_ASSETS_DIR)
            set(HYOSHI_ANDROID_ASSETS_DIR ${CMAKE_BINARY_DIR}/assets)
        endif()
        set(assetDir ${HYOSHI_ANDROID_ASSETS_DIR})
    else()
        add_executable(${target} ${APP_SOURCES})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME ${APP_OUTPUT_NAME})
        if(WIN32 AND APP_WINDOWS_RC)
            target_sources(${target} PRIVATE ${APP_WINDOWS_RC})
        endif()
        hyoshi_configure_executable(${target})
        set(assetDir $<TARGET_FILE_DIR:${target}>)
    endif()
    target_link_libraries(${target} PRIVATE hyoshi::engine)
    hyoshi_set_warnings(${target})

    file(GLOB engineTextures CONFIGURE_DEPENDS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../content/textures/*.png)
    set(commands
        COMMAND ${CMAKE_COMMAND} -E make_directory ${assetDir}/engine/fonts ${assetDir}/engine/textures
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HYOSHI_FONT_FILES} ${assetDir}/engine/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HYOSHI_FONT_LICENSE}
            ${assetDir}/engine/fonts/NotoSansJP-LICENSE.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${engineTextures} ${assetDir}/engine/textures
    )
    foreach(folder IN LISTS APP_ASSETS)
        cmake_path(ABSOLUTE_PATH folder BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        cmake_path(GET folder FILENAME name)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${folder} ${assetDir}/${name})
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD ${commands} VERBATIM)
endfunction()

# hyoshi_add_editor(<target> [OUTPUT_NAME <name>] SOURCES <file>... [ASSETS <folder>...] [WINDOWS_RC <file>])
# A game's editor (ADR 0002): an app like hyoshi_add_app's that also links the editor framework.
# Its sources hold a main that calls hyoshi::editor::Run, and the game's editor::Editor. Desktop
# only: on Android this does nothing.
function(hyoshi_add_editor target)
    if(ANDROID)
        return()
    endif()
    hyoshi_add_app(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE hyoshi::editor)
endfunction()
