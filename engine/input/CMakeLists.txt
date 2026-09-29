add_library(hyoshi_input STATIC
    src/InputQueue.cpp
    src/Trackpad.cpp
)
add_library(hyoshi::input ALIAS hyoshi_input)

target_include_directories(hyoshi_input PUBLIC include)
target_link_libraries(hyoshi_input PUBLIC hyoshi::core glm::glm)

hyoshi_set_warnings(hyoshi_input)
