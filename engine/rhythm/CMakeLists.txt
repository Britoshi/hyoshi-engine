add_library(hyoshi_rhythm STATIC
    src/ChartFile.cpp
    src/Judgment.cpp
    src/OffsetCalibration.cpp
    src/ScoreSystem.cpp
    src/ScrollMap.cpp
    src/SongClock.cpp
)
add_library(hyoshi::rhythm ALIAS hyoshi_rhythm)

target_include_directories(hyoshi_rhythm PUBLIC include)
target_link_libraries(hyoshi_rhythm
    PUBLIC hyoshi::core hyoshi::audio
    PRIVATE yyjson
)

hyoshi_set_warnings(hyoshi_rhythm)
