add_library(hyoshi_line STATIC
    src/LineChart.cpp
    src/LineJudge.cpp
    src/LinePlayfield.cpp
    src/PhigrosImport.cpp
    src/RpeImport.cpp
)
add_library(hyoshi::line ALIAS hyoshi_line)

target_include_directories(hyoshi_line PUBLIC include)
target_link_libraries(hyoshi_line PUBLIC hyoshi::core hyoshi::rhythm hyoshi::renderer)

hyoshi_set_warnings(hyoshi_line)
