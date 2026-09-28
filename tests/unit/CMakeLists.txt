add_executable(hyoshi_unit_tests
    TestMain.cpp
    AudioDecoderTests.cpp
    Camera2DTests.cpp
    ChartTests.cpp
    CircleTests.cpp
    ConcurrencyTests.cpp
    HandleTests.cpp
    ImageTests.cpp
    InputQueueTests.cpp
    ManiaJudgeTests.cpp
    MixerTests.cpp
    OffsetCalibrationTests.cpp
    OsuImportTests.cpp
    ResultTests.cpp
    SongClockTests.cpp
    SongLibraryTests.cpp
    TextTests.cpp
    UiTests.cpp
)

target_link_libraries(hyoshi_unit_tests PRIVATE hyoshi::core hyoshi::input hyoshi::renderer hyoshi::audio hyoshi::rhythm hyoshi::mania hyoshi::circle hyoshi::osu_import hyoshi::songs hyoshi::ui
    doctest::doctest hyoshi_stb)
list(GET HYOSHI_FONT_FILES 0 testFont)
target_compile_definitions(hyoshi_unit_tests PRIVATE HYOSHI_REPLAY_DIR="${PROJECT_SOURCE_DIR}/tests/replays"
    HYOSHI_TEST_FONT="${testFont}")
hyoshi_set_warnings(hyoshi_unit_tests)
hyoshi_configure_executable(hyoshi_unit_tests)

add_test(NAME hyoshi_unit_tests COMMAND hyoshi_unit_tests)
