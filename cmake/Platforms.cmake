# Per-platform settings for Hyoshi's executables.

# hyoshi_configure_executable(<target>)
# Windows: embeds platforms/windows/Utf8.manifest, so paths with non-ASCII characters (common in
# song and map names) survive the command line, environment variables, and the file APIs.
function(hyoshi_configure_executable target)
    if(WIN32)
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../platforms/windows/Utf8.manifest)
    endif()
endfunction()

# hyoshi_configure_web_app(<target> OUTPUT_NAME <name> [ASSETS <folder>...] [PRELOAD <src@dst>...])
# Web builds (Emscripten): <name>.js and .wasm, an index.html that starts them, and <name>.data, the
# files the app reads at run time, mounted at / as they'd sit next to a desktop executable (the
# engine's fonts and textures under engine/, each ASSETS folder under its own name). PRELOAD adds
# more, as Emscripten's --preload-file takes them (source@destination). Serve the folder with
# platforms/web/serve.py, or any server that sends the COOP and COEP headers threads need.
function(hyoshi_configure_web_app target)
    cmake_parse_arguments(PARSE_ARGV 1 WEB "" "OUTPUT_NAME" "ASSETS;PRELOAD")
    set(engineRoot ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..)
    set(webDir ${engineRoot}/platforms/web)

    set(files)
    foreach(font IN LISTS HYOSHI_FONT_FILES)
        cmake_path(GET font FILENAME name)
        list(APPEND files "${font}@/engine/fonts/${name}")
    endforeach()
    list(APPEND files "${engineRoot}/content/textures@/engine/textures")
    foreach(folder IN LISTS WEB_ASSETS)
        cmake_path(ABSOLUTE_PATH folder BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        cmake_path(GET folder FILENAME name)
        list(APPEND files "${folder}@/${name}")
    endforeach()
    list(APPEND files ${WEB_PRELOAD})
    set(preloadOptions)
    foreach(file IN LISTS files)
        list(APPEND preloadOptions "--preload-file=${file}")
    endforeach()

    target_link_options(${target} PRIVATE
        # Workers for the job system, started with the page so creating a thread never waits.
        "-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency"
        # The main loop yields to the browser once a frame (Application::Run), which needs JSPI.
        -sJSPI
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=2MB
        -sDEFAULT_PTHREAD_STACK_SIZE=1MB
        -sENVIRONMENT=web,worker
        "--pre-js=${webDir}/pre.js"
        # IndexedDB for the user data folder (pre.js), so settings and imported maps persist.
        -lidbfs.js
        $<$<CONFIG:Debug>:-sASSERTIONS=1>
        ${preloadOptions}
    )
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${webDir}/pre.js)
    set_target_properties(${target} PROPERTIES SUFFIX ".js")

    set(APP_TITLE ${WEB_OUTPUT_NAME})
    set(APP_OUTPUT_NAME ${WEB_OUTPUT_NAME})
    configure_file(${webDir}/index.html.in ${CMAKE_CURRENT_BINARY_DIR}/index.html @ONLY)
    configure_file(${webDir}/serve.py ${CMAKE_CURRENT_BINARY_DIR}/serve.py COPYONLY)
endfunction()
