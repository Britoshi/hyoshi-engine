add_library(hyoshi_ui STATIC
    src/Logo.cpp
    src/Ui.cpp
)
add_library(hyoshi::ui ALIAS hyoshi_ui)

target_include_directories(hyoshi_ui PUBLIC include)
target_link_libraries(hyoshi_ui PUBLIC hyoshi::core hyoshi::input hyoshi::renderer hyoshi::rhi
    PRIVATE hyoshi::platform)

hyoshi_set_warnings(hyoshi_ui)
