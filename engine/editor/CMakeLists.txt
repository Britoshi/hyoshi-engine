# The editor framework (ADR 0002). Games link it only through hyoshi_add_editor, never through
# hyoshi::engine, so shipped builds contain no editor code.
add_library(hyoshi_editor STATIC
    src/Editor.cpp
)
add_library(hyoshi::editor ALIAS hyoshi_editor)

target_include_directories(hyoshi_editor PUBLIC include)
target_link_libraries(hyoshi_editor PUBLIC hyoshi::core hyoshi::app hyoshi::debug PRIVATE hyoshi::platform)

hyoshi_set_warnings(hyoshi_editor)
