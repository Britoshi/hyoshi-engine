# clang-format and clang-tidy targets. Homebrew's LLVM is keg-only, so look there too, and on
# Windows in the LLVM tools that Visual Studio installs (found from its developer environment).
set(HYOSHI_LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
if(DEFINED ENV{VCINSTALLDIR})
    cmake_path(SET vcInstallDir NORMALIZE "$ENV{VCINSTALLDIR}")
    list(APPEND HYOSHI_LLVM_HINTS ${vcInstallDir}Tools/Llvm/x64/bin)
endif()

find_program(HYOSHI_CLANG_FORMAT clang-format HINTS ${HYOSHI_LLVM_HINTS})
find_program(HYOSHI_CLANG_TIDY clang-tidy HINTS ${HYOSHI_LLVM_HINTS})

# hyoshi_add_code_checks(<folder>...)
# Adds `format`, `format-check`, and `tidy` targets over the .h and .cpp files in the folders
# (relative to the calling project). Call it from the top-level project only: the engine does for
# its own sources, and a game for its own. Style comes from the .clang-format and .clang-tidy
# files above each source.
function(hyoshi_add_code_checks)
    set(patterns)
    foreach(folder IN LISTS ARGN)
        list(APPEND patterns ${PROJECT_SOURCE_DIR}/${folder}/*.h ${PROJECT_SOURCE_DIR}/${folder}/*.cpp)
    endforeach()
    file(GLOB_RECURSE formatSources CONFIGURE_DEPENDS ${patterns})
    set(tidySources ${formatSources})
    list(FILTER tidySources INCLUDE REGEX "\.cpp$")

    if(HYOSHI_CLANG_FORMAT)
        add_custom_target(format
            COMMAND ${HYOSHI_CLANG_FORMAT} -i ${formatSources}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Formatting sources with clang-format"
            VERBATIM
        )
        add_custom_target(format-check
            COMMAND ${HYOSHI_CLANG_FORMAT} --dry-run --Werror ${formatSources}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Checking formatting with clang-format"
            VERBATIM
        )
    endif()

    if(HYOSHI_CLANG_TIDY)
        add_custom_target(tidy
            COMMAND ${HYOSHI_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet --warnings-as-errors=* ${tidySources}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Checking naming with clang-tidy"
            VERBATIM
        )
    endif()
endfunction()
