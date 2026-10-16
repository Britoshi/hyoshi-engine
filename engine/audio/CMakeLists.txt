add_library(hyoshi_audio STATIC
    src/Mixer.cpp
    src/MusicStream.cpp
    src/Synth.cpp
    miniaudio/MiniaudioBackend.cpp
    miniaudio/MiniaudioDecoder.cpp
    miniaudio/Mp3Backend.cpp
    miniaudio/ThirdPartyImplementations.cpp
)
add_library(hyoshi::audio ALIAS hyoshi_audio)

target_include_directories(hyoshi_audio PUBLIC include PRIVATE miniaudio)
target_link_libraries(hyoshi_audio
    PUBLIC hyoshi::core
    PRIVATE hyoshi_miniaudio
)

# ASIO drivers (Windows) are COM objects found in the registry.
if(WIN32)
    target_sources(hyoshi_audio PRIVATE asio/AsioOutput.cpp)
    target_include_directories(hyoshi_audio PRIVATE asio)
    target_link_libraries(hyoshi_audio PRIVATE ole32 advapi32 user32)
endif()

# miniaudio loads AAudio and OpenSL ES at runtime on Android; Apple frameworks are linked directly.
if(APPLE)
    target_link_libraries(hyoshi_audio PRIVATE "-framework CoreFoundation" "-framework CoreAudio"
                                               "-framework AudioToolbox")
endif()

# The decoders (miniaudio's, its resampler, and stb_vorbis) are optimized in debug builds too (see
# Mp3Backend.cpp and ThirdPartyImplementations.cpp, which do it for MSVC): unoptimized, opening a
# song, finding a preview's place in it, and decoding ahead all take several times longer.
if(NOT MSVC)
    set_source_files_properties(miniaudio/Mp3Backend.cpp miniaudio/ThirdPartyImplementations.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

hyoshi_set_warnings(hyoshi_audio)
