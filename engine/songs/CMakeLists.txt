add_library(hyoshi_songs STATIC
    src/Charts.cpp
    src/SongLibrary.cpp
    src/SongPlayer.cpp
)
add_library(hyoshi::songs ALIAS hyoshi_songs)

target_include_directories(hyoshi_songs PUBLIC include)
target_link_libraries(hyoshi_songs PUBLIC hyoshi::core hyoshi::audio hyoshi::rhythm hyoshi::mania hyoshi::circle
    PRIVATE hyoshi::platform hyoshi::osu_import)

hyoshi_set_warnings(hyoshi_songs)
