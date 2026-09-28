add_library(hyoshi_app STATIC
    src/Application.cpp
    src/ClockDemo.cpp
    src/SplashScene.cpp
)
add_library(hyoshi::app ALIAS hyoshi_app)

target_include_directories(hyoshi_app PUBLIC include PRIVATE src)
target_link_libraries(hyoshi_app PUBLIC hyoshi::core hyoshi::input hyoshi::platform hyoshi::rhi hyoshi::renderer
    hyoshi::debug hyoshi::audio hyoshi::rhythm hyoshi::songs hyoshi::ui)

hyoshi_set_warnings(hyoshi_app)
