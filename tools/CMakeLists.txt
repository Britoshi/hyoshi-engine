add_subdirectory(osu-import)
# Desktop tools; the Android build only needs the importer's library.
if(NOT ANDROID)
    add_subdirectory(asset-cooker)
endif()
