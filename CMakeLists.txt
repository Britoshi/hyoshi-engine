cmake_minimum_required(VERSION 3.28)

project(HyoshiEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "Hyoshi builds for x64 only. Configure from an x64 developer environment "
                            "(\"x64 Native Tools Command Prompt\", or Launch-VsDevShell.ps1 -Arch amd64).")
    endif()
    # For the engine and its dependencies. Games get the same through hyoshi::core.
    add_compile_options(/utf-8 /Zc:preprocessor $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
endif()

# Built on the engine's own; a game that adds the engine as a subdirectory gets neither by default.
option(HYOSHI_BUILD_TESTS "Build the engine's unit tests" ${PROJECT_IS_TOP_LEVEL})
option(HYOSHI_BUILD_SAMPLES "Build the sample app" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)
include(Dependencies)
include(CompilerWarnings)
include(Platforms)
include(Tooling)
include(Shaders)
include(HyoshiApp)

add_subdirectory(engine)
add_subdirectory(modes)
add_subdirectory(tools)

if(HYOSHI_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(HYOSHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PROJECT_IS_TOP_LEVEL)
    hyoshi_add_code_checks(engine modes tools tests samples)
endif()
