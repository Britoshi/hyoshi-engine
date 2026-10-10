add_subdirectory(osu-import)
# Desktop tools; the Android and web builds only need the importer's library.
if(NOT ANDROID AND NOT EMSCRIPTEN)
    add_subdirectory(asset-cooker)
endif()
