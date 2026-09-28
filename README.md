# Hyoshi Engine

A mobile-first C++20 engine for rhythm games.

Hyoshi is built on one premise: **the audio clock is the source of truth.** Song time comes from the audio device, input is timestamped by the OS, and judgment is integer-microsecond math, so results are deterministic and replays reproduce exactly. Rendering is Vulkan behind a small RHI, and every game style (Mania, Drum, Circle, Highway, Line) is meant to be a pluggable mode; Mania is the first.

- **Targets:** Android (Vulkan) and macOS (Vulkan through MoltenVK). Windows x64 (MSVC) builds and runs as a development host.
- **Later:** iOS (Metal), Windows as a shipping target, Linux.
- **License:** MIT (see [LICENSE](LICENSE)). The fonts it installs, Noto Sans JP, are under the SIL Open Font License 1.1; other dependencies keep their own licenses ([docs/REFERENCES.md](docs/REFERENCES.md)).

The first game on it is developed separately, in its own repository.

## What's in it

| Module | What it does |
|---|---|
| `core` | Logging, asserts, `Result<T>`, generational handles, integer time math, a lock-free SPSC queue and seqlock, a job system, environment-variable helpers |
| `platform` | SDL3: the window, input with OS timestamps, the app lifecycle (Android backgrounding), the host clock, file and asset access, safe area, screen orientation |
| `input` | Input events and a queue sorted by host time |
| `rhi` | A small render hardware interface and its Vulkan backend (volk, VMA; MoltenVK on macOS): swapchain with pre-rotation, pipeline cache, validation in debug builds |
| `renderer` | `Camera2D` (the short side is 1080 units, so layouts know the orientation), `SpriteBatch`, `TextRenderer` (TrueType and OpenType, including CJK, as runtime distance-field glyphs), images and distance fields from PNG/JPEG |
| `audio` | `IAudioBackend` on miniaudio (CoreAudio, AAudio, WASAPI), a mixer in the audio callback, decoding (WAV, MP3, FLAC, Ogg Vorbis), sample-exact scheduling on music frames |
| `rhythm` | `SongClock` (song time anchored to the audio cursor, filtered, never rewinds), the chart format (`.rchart.json`), scroll maps, judgments, scoring, offset calibration |
| `modes/mania` | Mania charts, the judge (with autoplay), and the playfield |
| `songs` | Chart loading (`.rchart.json`, and osu!mania `.osu` converted in memory), a song library that scans song folders, and `SongPlayer`, which ties music to the song clock |
| `ui` | An immediate-mode UI kit for game menus (buttons, toggles, sliders, choices, lists with scrolling; mouse, touch, and keyboard), and logos drawn as distance fields |
| `app` | The application: the main loop, scenes and fades, the debug overlay (Dear ImGui, F1), development hooks, and the "Made with Hyoshi Engine" splash |
| `debug` | Dear ImGui drawn through the RHI |
| `editor` | The editor framework (desktop): a menu bar and dockable ImGui panels over the running game, which a game's editor adds to ([ADR 0002](docs/decisions/0002-editor-framework.md)) |
| `tools/osu-import` | `hyoshi-osu-import`: osu!mania beatmaps to `.rchart.json` |
| `tools/asset-cooker` | `hyoshi-asset-cooker`: app icons (window, Windows `.ico`, Android launcher and Play Store) from a logo |

The design, milestones, and what was built where it differs from the plan are in [docs/DESIGN.md](docs/DESIGN.md). [docs/HANDOVER.md](docs/HANDOVER.md) has the current state, what is and isn't verified, invariants, and gotchas.

## Status

- **M0 Foundation:** done on macOS and Windows; Android builds and runs on the emulator (not yet on a phone). Tracy, CI, and ADR 0001 are still to do.
- **M1 Vulkan bring-up:** done on macOS and Windows, validation clean, including surface loss and resize stress. Runs on the Android emulator.
- **M2 2D renderer:** sprites (10,000 at 120 Hz on an M5 Max), text, distance-field images, safe area. Still to come: texture atlases, MSDF text, ASTC.
- **M3 Audio and the song clock:** done and stress-tested on macOS and Windows (device stalls, random seeks). Plays on the Android emulator.
- **M4 First playable (Mania):** chart format, judge, replays, autoplay, osu!mania import, song library, UI kit. The first game is built on them.

## Building

| Tool | Needed for | Install |
|---|---|---|
| CMake 3.28+ and Ninja | Building | `brew install cmake ninja`, or `winget install Kitware.CMake Ninja-build.Ninja` |
| macOS: Xcode Command Line Tools; Vulkan loader, MoltenVK, validation layers | Compiler, running | `xcode-select --install`; `brew install vulkan-loader molten-vk vulkan-validationlayers vulkan-tools spirv-tools` |
| Windows: Visual Studio 2022 or 2026, "Desktop development with C++"; optionally the Vulkan SDK | MSVC; validation layers in debug builds | Visual Studio Installer; `winget install KhronosGroup.VulkanSDK` |
| LLVM 19+ (clang-format, clang-tidy) | Code checks | `brew install llvm` (Visual Studio installs them on Windows) |
| `slangc` | Shaders | Nothing to install: the build downloads a pinned release |

Dependencies (SDL3, spdlog, doctest, volk, VMA, glm, Dear ImGui, miniaudio, yyjson, stb, the fonts) come through CPM with pinned versions and are cached in `~/.cache/CPM`.

```sh
cmake --preset macos-debug            # or windows-debug, from an x64 Visual Studio developer environment
cmake --build --preset macos-debug
ctest --preset macos-debug            # unit tests
./build/macos-debug/samples/metronome/HyoshiMetronome
```

On Windows, configure and build from an x64 developer environment (the "x64 Native Tools Command Prompt for VS", or PowerShell after `Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64`); the sample is `build\windows-debug\samples\metronome\HyoshiMetronome.exe`. Use the `-release` presets for optimized builds.

**The sample** (`samples/metronome`) is the smallest game on the engine: the splash, then a metronome whose beat flash follows the audio clock. F1 shows the clock's debug panel.

**Code checks:** `cmake --build --preset <preset> --target format` (or `format-check`, `tidy`). Naming: PascalCase types, functions, and public members; camelCase locals and private members; UPPER_CASE constants.

### Development hooks

Debug builds enable the Vulkan validation layer, and an app exits with a failure code if validation reported anything, song time went backwards, or a scene reported a failed check. Environment variables for scripted checks, in any app on the engine:

| Variable | Effect |
|---|---|
| `HYOSHI_EXIT_AFTER_MS=3000` | Quit after 3 seconds |
| `HYOSHI_SCREENSHOT=out.png` | Save frame 30 as a PNG (`HYOSHI_SCREENSHOT_FRAME=400` for another frame) |
| `HYOSHI_SCREENSHOT_MS=5000` | Take the screenshot this long after startup instead (the frame rate is uncapped while the display sleeps) |
| `HYOSHI_WINDOW_SIZE=720x1280` | Open the window at this size, e.g. portrait |
| `HYOSHI_DEBUG_UI=1` | Show the debug overlay from the start (F1 toggles it) |
| `HYOSHI_SCENE=clock` | Run the clock demo instead of the game |
| `HYOSHI_STRESS_SURFACE=1` | Every 10 frames, alternately resize the window and destroy and recreate the surface |
| `HYOSHI_STRESS_AUDIO=1` | Stop the audio device for the last 200 ms of every 3 seconds |
| `HYOSHI_STRESS_SEEK=1` | Clock demo: seek to a random position every 1.5 seconds |
| `HYOSHI_SPRITES=10000` | Add bouncing sprites, for renderer stress tests |
| `HYOSHI_MUSIC=song.ogg`, `HYOSHI_BPM=128`, `HYOSHI_FIRST_BEAT_MS=350` | Clock demo: play a file instead of the generated metronome track, at this tempo |

## Making a game

A game lives in its own repository and adds the engine as a git submodule. The layout the engine's tooling expects:

```
my-game/
  CMakeLists.txt       add_subdirectory(hyoshi-engine), then hyoshi_add_app(...)
  hyoshi-engine/       this repository, as a submodule
  src/                 the game: a hyoshi::app::Game and its scenes
  content/textures/    the game's assets (app-icon.png: the window icon)
  content/charts/      local maps for development (bundled into debug APKs)
  windows/app.rc       the executable's icon (app.ico)
  android/             the Gradle project (see below)
```

The build:

```cmake
cmake_minimum_required(VERSION 3.28)
project(MyGame LANGUAGES C CXX)
add_subdirectory(hyoshi-engine)
hyoshi_add_app(my_game
    OUTPUT_NAME MyGame
    SOURCES src/Main.cpp src/MyGame.cpp
    ASSETS ${PROJECT_SOURCE_DIR}/content/textures
    WINDOWS_RC ${PROJECT_SOURCE_DIR}/windows/app.rc)
hyoshi_add_code_checks(src)
```

`hyoshi_add_app` builds an executable on desktops and `libmain.so` on Android, links every engine module, and installs the engine's fonts and logos (under `engine/`) and each `ASSETS` folder next to the executable or into the APK.

The code: a `hyoshi::app::Game` sets itself up once the window, GPU, and audio exist, and hands the Application its first scene. Scenes derive from `hyoshi::app::Scene`, draw through `hyoshi::ui::Ui` each frame, and move on with `SwitchTo(std::make_unique<NextScene>(...))`. `samples/metronome/Main.cpp` is a complete example:

```cpp
class MyGame : public hyoshi::app::Game
{
public:
    hyoshi::Result<void> Initialize(hyoshi::app::AppServices& services) override;   // settings, content
    std::unique_ptr<hyoshi::app::Scene> CreateFirstScene() override;                 // after the splash
    void Update() override;                                                          // every frame
    void Save() override;                                                            // backgrounding, exit
};

int main(int, char*[])
{
    hyoshi::log::Initialize();
    MyGame game;
    hyoshi::app::AppConfig config;
    config.Name = "My Game";                          // window title and user data folder
    config.WindowIconPath = "textures/app-icon.png";
    int exitCode = hyoshi::app::Application(config, game).Run();
    hyoshi::log::Shutdown();
    return exitCode;
}
```

**The game's editor.** A game can also build an editor: the game itself, run with a menu bar and dockable ImGui panels over it, to which the game adds its own panels ([ADR 0002](docs/decisions/0002-editor-framework.md)). The game's code goes in a library that both executables link, and `hyoshi_add_editor` (desktop only; nothing on Android) builds the editor like `hyoshi_add_app` plus the editor framework, which the game's own executable never links:

```cmake
add_library(my_game_code STATIC src/MyGame.cpp)
target_link_libraries(my_game_code PUBLIC hyoshi::engine)
hyoshi_add_app(my_game OUTPUT_NAME MyGame SOURCES src/Main.cpp)
target_link_libraries(my_game PRIVATE my_game_code)
hyoshi_add_editor(my_game_editor OUTPUT_NAME MyGameEditor SOURCES src/editor/EditorMain.cpp src/editor/MyEditor.cpp)
if(TARGET my_game_editor)
    target_link_libraries(my_game_editor PRIVATE my_game_code)
endif()
```

```cpp
class MyPanel : public hyoshi::editor::Panel
{
public:
    MyPanel() : Panel("My panel") {}
    void Build() override { ImGui::Text("..."); }    // the window's contents, every frame it's open
};

class MyEditor : public hyoshi::editor::Editor
{
public:
    hyoshi::Result<void> Initialize(hyoshi::editor::EditorServices& services) override
    {
        services.AddPanel(std::make_unique<MyPanel>());   // after the game's Initialize
        return {};
    }
};

// EditorMain.cpp: the game's name, so the editor shares its settings; no splash.
config.Name = "My Game";
config.WindowTitle = "My Game Editor";
int exitCode = hyoshi::editor::Run(config, game, editor);
```

Game code can check `AppServices::IsEditor` to skip what only suits players. The editor saves its panel layout in `editor-imgui.ini`, next to the game's settings.

`hyoshi-asset-cooker icons <logo.png> <game folder>` writes a game's icons into that layout: `content/textures/app-icon.png`, `windows/app.ico`, and the Android launcher and Play Store icons.

### Android

A game's `android/app/build.gradle` sets its identity and applies the engine's `platforms/android/hyoshi-app.gradle`, which does the rest: the native build with the pinned NDK and CMake, SDL's Java glue and the engine's activity (`com.britoshi.hyoshi.HyoshiActivity`), assets, and, in debug builds only, the game's `content/charts` bundled into the APK and unpacked on first launch.

```groovy
plugins { id 'com.android.application' }
android {
    namespace = 'com.example.mygame'
    defaultConfig {
        applicationId = 'com.example.mygame'
        versionCode = 1
        versionName = '0.1.0'
        externalNativeBuild { cmake { targets 'my_game' } }
    }
}
ext.hyoshiEngineDir = file('../../hyoshi-engine')
ext.hyoshiGameDir = file('../..')
apply from: "${hyoshiEngineDir}/platforms/android/hyoshi-app.gradle"
```

The manifest names the engine's activity, `com.britoshi.hyoshi.HyoshiActivity`, and requires `android.hardware.vulkan.version` 0x401000. Building needs a JDK 17+, the Android SDK (platform 36), NDK `27.3.13750724`, and the SDK's CMake `4.1.2` (`sdkmanager "ndk;27.3.13750724" "cmake;4.1.2"`); then `gradlew assembleDebug` in the game's `android` folder.

## Repository layout

| Path | Contents |
|---|---|
| `engine/` | The engine's modules (table above), each a CMake target `hyoshi::<module>`; `hyoshi::engine` is all of them |
| `modes/mania/` | The Mania mode |
| `tools/` | `osu-import`, `asset-cooker` |
| `samples/metronome/` | The sample app |
| `shaders/` | Slang sources, compiled to SPIR-V 1.3 and embedded at build time |
| `content/textures/` | The engine's logos, for the splash |
| `platforms/` | Per-platform glue: the Windows UTF-8 manifest, SDL's Android Java sources and the engine's activity, `hyoshi-app.gradle` |
| `cmake/` | CPM, pinned dependencies, warnings, shaders, `hyoshi_add_app`, `hyoshi_add_editor`, code checks |
| `tests/` | doctest unit tests and replay fixtures |
| `docs/` | Design, references, handover, architecture decision records (`decisions/`) |
