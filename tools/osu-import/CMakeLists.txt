add_library(hyoshi_osu_import STATIC
    src/OsuManiaImporter.cpp
    src/OsuStandardImporter.cpp
)
add_library(hyoshi::osu_import ALIAS hyoshi_osu_import)
target_include_directories(hyoshi_osu_import PUBLIC include)
target_link_libraries(hyoshi_osu_import PUBLIC hyoshi::mania hyoshi::circle)
hyoshi_set_warnings(hyoshi_osu_import)

# The command-line converter is a desktop tool.
if(NOT ANDROID)
    add_executable(hyoshi_osu_import_tool src/Main.cpp)
    set_target_properties(hyoshi_osu_import_tool PROPERTIES OUTPUT_NAME hyoshi-osu-import)
    target_link_libraries(hyoshi_osu_import_tool PRIVATE hyoshi::osu_import)
    hyoshi_set_warnings(hyoshi_osu_import_tool)
    hyoshi_configure_executable(hyoshi_osu_import_tool)
endif()
