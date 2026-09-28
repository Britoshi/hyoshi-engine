add_library(hyoshi_mania STATIC
    src/ManiaChart.cpp
    src/ManiaJudge.cpp
    src/ManiaPlayfield.cpp
)
add_library(hyoshi::mania ALIAS hyoshi_mania)

target_include_directories(hyoshi_mania PUBLIC include)
target_link_libraries(hyoshi_mania PUBLIC hyoshi::core hyoshi::rhythm hyoshi::renderer)

hyoshi_set_warnings(hyoshi_mania)
