add_library(hyoshi_core STATIC
    src/Assert.cpp
    src/Env.cpp
    src/JobSystem.cpp
    src/Log.cpp
)
add_library(hyoshi::core ALIAS hyoshi_core)

find_package(Threads REQUIRED)

target_include_directories(hyoshi_core PUBLIC include)
target_link_libraries(hyoshi_core PUBLIC spdlog::spdlog Threads::Threads)

# Everything built on the engine, games included: C++20, and with MSVC, UTF-8 sources (fmt requires
# it), the standard preprocessor (for __VA_OPT__), and a truthful __cplusplus.
target_compile_features(hyoshi_core PUBLIC cxx_std_20)
target_compile_options(hyoshi_core PUBLIC
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8;/Zc:preprocessor;/Zc:__cplusplus>)

# Trace and debug logging compile out of release builds.
target_compile_definitions(hyoshi_core PUBLIC
    SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,SPDLOG_LEVEL_TRACE,SPDLOG_LEVEL_INFO>
)

if(ANDROID)
    target_link_libraries(hyoshi_core PRIVATE log)
endif()

hyoshi_set_warnings(hyoshi_core)
