add_library(hyoshi_circle STATIC
    src/CircleChart.cpp
    src/CircleJudge.cpp
    src/CirclePlayfield.cpp
    src/CircleScore.cpp
)
add_library(hyoshi::circle ALIAS hyoshi_circle)

target_include_directories(hyoshi_circle PUBLIC include)
target_link_libraries(hyoshi_circle PUBLIC hyoshi::core hyoshi::rhythm hyoshi::renderer)

hyoshi_set_warnings(hyoshi_circle)
