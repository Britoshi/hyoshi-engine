# Per-platform settings for Hyoshi's executables.

# hyoshi_configure_executable(<target>)
# Windows: embeds platforms/windows/Utf8.manifest, so paths with non-ASCII characters (common in
# song and map names) survive the command line, environment variables, and the file APIs.
function(hyoshi_configure_executable target)
    if(WIN32)
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../platforms/windows/Utf8.manifest)
    endif()
endfunction()
