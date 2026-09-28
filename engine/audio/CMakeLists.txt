add_library(hyoshi_audio STATIC
    src/Mixer.cpp
    src/Synth.cpp
    miniaudio/MiniaudioBackend.cpp
    miniaudio/MiniaudioDecoder.cpp
    miniaudio/ThirdPartyImplementations.cpp
)
add_library(hyoshi::audio ALIAS hyoshi_audio)

target_include_directories(hyoshi_audio PUBLIC include PRIVATE miniaudio)
target_link_libraries(hyoshi_audio
    PUBLIC hyoshi::core
    PRIVATE hyoshi_miniaudio
)

# miniaudio loads AAudio and OpenSL ES at runtime on Android; Apple frameworks are linked directly.
if(APPLE)
    target_link_libraries(hyoshi_audio PRIVATE "-framework CoreFoundation" "-framework CoreAudio"
                                               "-framework AudioToolbox")
endif()

hyoshi_set_warnings(hyoshi_audio)
