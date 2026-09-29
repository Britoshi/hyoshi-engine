<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/logo-dark.png">
    <img alt="Hyoshi Engine" src="docs/images/logo-light.png" width="480">
  </picture>
</p>

<p align="center">
  A C++20 game engine for rhythm games, for Android, macOS, and Windows.
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue.svg"></a>
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C.svg">
  <img alt="Vulkan" src="https://img.shields.io/badge/graphics-Vulkan-AC162C.svg">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-Android%20%7C%20macOS%20%7C%20Windows-lightgrey.svg">
</p>

---

Hyoshi is an engine for rhythm games. Song time comes from the audio device's playback position, input events carry the operating system's timestamps, and hits are judged in integer microseconds. The same chart and the same inputs always produce the same result, so replays play back exactly.

It renders with Vulkan (MoltenVK on macOS) and uses SDL3 for windowing and input and miniaudio for sound. Each style of rhythm game is a separate module: Mania and Circle are included, and Drum, Highway, and Line are planned.

> [!WARNING]
> Hyoshi is in early development. APIs change without notice, there are no releases yet, and Android has only been tested on the emulator.

## Features

- **Audio-driven timing.** `SongClock` follows the audio device's playback position. It never runs backwards, and it recovers from device stalls and seeks.
- **Deterministic judgment and replays.** Judgment depends only on the chart and the timestamped inputs, so a replay reproduces the original run exactly.
- **Game modes as modules.** Mania (vertical lanes, like osu!mania) and Circle (hit circles, sliders, and spinners, like osu!standard), each with a judge, scoring, autoplay, and a playfield renderer.
- **osu! map import.** osu!mania and osu!standard `.osu` files load directly. `hyoshi-osu-import` converts osu!mania maps to Hyoshi's JSON chart format.
- **2D renderer.** Sprite batching, distance-field text for any TrueType or OpenType font (including Japanese), safe areas, and Android screen rotation.
- **Audio.** WAV, MP3, FLAC, and Ogg Vorbis; a mixer that runs in the audio callback; sounds scheduled to exact sample positions in the music.
- **Game UI.** Immediate-mode buttons, toggles, sliders, and scrolling lists for menus, driven by mouse, touch, or keyboard.
- **Editor framework.** A game can build a desktop editor: the game itself, with dockable Dear ImGui panels over it. Release builds contain no editor code.
- **Debug tools.** An ImGui overlay (F1), Vulkan validation in debug builds, and environment variables for screenshots, timed exits, and stress tests.

## Platform support

| Platform | Graphics | Status |
|---|---|---|
| Android 8.0+ (arm64-v8a, x86_64) | Vulkan 1.1 | Builds and runs on the emulator. Not yet tested on a phone. |
| macOS (Apple Silicon) | Vulkan through MoltenVK | Builds and runs. Debug builds report Vulkan validation errors ([known issue](docs/DEVELOPMENT.md#known-issues)). |
| Windows (x64) | Vulkan | Builds and runs. Used for development; not yet a release target. |
| iOS | Metal | Planned |
| Linux | Vulkan | Planned |

## Getting started

### Requirements

| Tool | Notes |
|---|---|
| CMake 3.28+ and Ninja | `brew install cmake ninja`, or `winget install Kitware.CMake Ninja-build.Ninja` |
| A C++20 compiler | macOS: Xcode Command Line Tools (`xcode-select --install`). Windows: Visual Studio 2022 or later with "Desktop development with C++". |
| Vulkan runtime | macOS: `brew install vulkan-loader molten-vk vulkan-validationlayers vulkan-tools spirv-tools`. Windows: your GPU driver; the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) adds validation layers for debug builds. |
| LLVM 19+ (optional) | `clang-format` and `clang-tidy` for the code checks. `brew install llvm`; Visual Studio includes them. |

All other dependencies, including the Slang shader compiler, are downloaded at configure time with pinned versions through [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) and cached in `~/.cache/CPM`.

### Build

```sh
git clone https://github.com/Britoshi/hyoshi-engine.git
cd hyoshi-engine
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
./build/macos-debug/samples/metronome/HyoshiMetronome
```

On Windows, use the `windows-debug` preset from an x64 developer environment: the "x64 Native Tools Command Prompt for VS", or PowerShell after `Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64`. The sample is then `build\windows-debug\samples\metronome\HyoshiMetronome.exe`.

| Preset | Platform |
|---|---|
| `macos-debug`, `macos-release` | macOS, Apple Silicon |
| `windows-debug`, `windows-release` | Windows x64, MSVC |

The sample in `samples/metronome` is the smallest possible game: the splash screen, then a metronome whose flash follows the audio clock. Press F1 for the debug overlay.

## Making a game

A game lives in its own repository and includes the engine as a git submodule:

```
my-game/
  CMakeLists.txt       add_subdirectory(hyoshi-engine), then hyoshi_add_app(...)
  hyoshi-engine/       this repository, as a submodule
  src/                 the game's code
  content/textures/    the game's assets (app-icon.png is the window icon)
  content/charts/      maps for development (bundled into debug APKs)
  windows/app.rc       the Windows executable's icon
  android/             the Gradle project
```

### CMake

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

`hyoshi_add_app` builds an executable on desktop and `libmain.so` on Android, links every engine module, and installs the engine's fonts and logos and each `ASSETS` folder next to the executable or into the APK.

### Code

A game implements `hyoshi::app::Game`. The engine calls `Initialize` once the window, GPU, and audio are ready, then asks for the first scene. Scenes derive from `hyoshi::app::Scene`, draw through `hyoshi::ui::Ui` each frame, and change with `SwitchTo(std::make_unique<NextScene>(...))`.

```cpp
class MyGame : public hyoshi::app::Game
{
public:
    hyoshi::Result<void> Initialize(hyoshi::app::AppServices& services) override;   // load settings and content
    std::unique_ptr<hyoshi::app::Scene> CreateFirstScene() override;                 // shown after the splash
    void Update() override;                                                          // called every frame
    void Save() override;                                                            // on backgrounding and exit
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

[`samples/metronome/Main.cpp`](samples/metronome/Main.cpp) is a complete example.

### Editor

A game can also build a desktop editor: the game runs with a menu bar and dockable ImGui panels over it, and the game adds its own panels. Put the game's code in a library that both executables link, and build the editor with `hyoshi_add_editor`. It does nothing on Android, and the game's own executable never links the editor.

```cmake
add_library(my_game_code STATIC src/MyGame.cpp)
target_link_libraries(my_game_code PUBLIC hyoshi::engine)

hyoshi_add_app(my_game OUTPUT_NAME MyGame SOURCES src/Main.cpp)
target_link_libraries(my_game PRIVATE my_game_code)

hyoshi_add_editor(my_game_editor OUTPUT_NAME MyGameEditor
    SOURCES src/editor/EditorMain.cpp src/editor/MyEditor.cpp)
if(TARGET my_game_editor)
    target_link_libraries(my_game_editor PRIVATE my_game_code)
endif()
```

```cpp
class MyPanel : public hyoshi::editor::Panel
{
public:
    MyPanel() : Panel("My panel") {}
    void Build() override { ImGui::Text("..."); }    // the panel's contents, every frame it's open
};

class MyEditor : public hyoshi::editor::Editor
{
public:
    hyoshi::Result<void> Initialize(hyoshi::editor::EditorServices& services) override
    {
        services.AddPanel(std::make_unique<MyPanel>());   // runs after the game's Initialize
        return {};
    }
};

// EditorMain.cpp: use the game's name so the editor shares its settings.
config.Name = "My Game";
config.WindowTitle = "My Game Editor";
int exitCode = hyoshi::editor::Run(config, game, editor);
```

Game code can check `AppServices::IsEditor` to skip things that only make sense for players. The editor saves its panel layout in `editor-imgui.ini`, next to the game's settings. The design is described in [docs/decisions/0002-editor-framework.md](docs/decisions/0002-editor-framework.md).

### Android

A game's `android/app/build.gradle` sets the app's identity and applies the engine's [`platforms/android/hyoshi-app.gradle`](platforms/android/hyoshi-app.gradle), which handles the native build with the pinned NDK and CMake, SDL's Java code and the engine's activity, and assets. Debug builds also bundle the game's `content/charts` into the APK.

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

The manifest uses the activity `com.britoshi.hyoshi.HyoshiActivity` and requires `android.hardware.vulkan.version` `0x401000` (Vulkan 1.1).

To build, install JDK 17+, the Android SDK (platform 36), NDK `27.3.13750724`, and the SDK's CMake `4.1.2` (`sdkmanager "ndk;27.3.13750724" "cmake;4.1.2"`), then run `gradlew assembleDebug` in the game's `android` folder.

### Tools

| Tool | Usage |
|---|---|
| `hyoshi-osu-import` | `hyoshi-osu-import <input.osu> <output.rchart.json> [--force]` converts an osu!mania map to Hyoshi's chart format. |
| `hyoshi-asset-cooker` | `hyoshi-asset-cooker icons <logo.png> <game folder>` writes a game's icons from its logo: the window icon, `windows/app.ico`, and the Android launcher and Play Store icons. |

## Project layout

| Path | Contents |
|---|---|
| `engine/core` | Logging, asserts, `Result<T>`, handles, integer time math, lock-free queue and seqlock, job system |
| `engine/platform` | SDL3 window, timestamped input, app lifecycle, host clock, file and asset access, safe area, orientation |
| `engine/input` | Input events, a queue sorted by timestamp, and trackpad-as-tablet mapping |
| `engine/rhi` | The render hardware interface and its Vulkan backend (volk, VMA) |
| `engine/renderer` | `Camera2D`, `SpriteBatch`, `TextRenderer`, image loading and distance fields |
| `engine/audio` | The audio backend interface, miniaudio backend, mixer, and decoders |
| `engine/rhythm` | `SongClock`, the chart format (`.rchart.json`), scroll maps, judgment, scoring, offset calibration |
| `engine/songs` | Chart loading, the song library, and `SongPlayer`, which ties music to the song clock |
| `engine/ui` | The immediate-mode game UI and logo rendering |
| `engine/app` | The main loop, scenes and transitions, the debug overlay, and the splash screen |
| `engine/debug` | Dear ImGui drawn through the RHI |
| `engine/editor` | The editor framework (desktop only) |
| `modes/` | Game modes: `mania`, `circle` |
| `tools/` | `osu-import`, `asset-cooker` |
| `samples/` | The metronome sample |
| `shaders/` | Slang shaders, compiled to SPIR-V at build time |
| `platforms/` | Windows manifest, Android Java code and Gradle script |
| `cmake/` | Dependencies, compiler warnings, shader compilation, `hyoshi_add_app`, `hyoshi_add_editor`, code checks |
| `tests/` | Unit tests (doctest) and replay fixtures |
| `docs/` | Documentation |

Each engine module is a CMake target named `hyoshi::<module>`. `hyoshi::engine` links all of them.

## Documentation

- [Architecture](docs/ARCHITECTURE.md): how the modules fit together, the main loop, and how time flows from the audio device to judgment.
- [Development](docs/DEVELOPMENT.md): debug environment variables, stress tests, platform notes, and known issues.
- [Design document](docs/DESIGN.md): the full design and roadmap.
- [Decision records](docs/decisions/): the reasoning behind major design choices.
- [References](docs/REFERENCES.md): pinned dependency versions and further reading.

## Roadmap

- Fix the Vulkan validation errors on macOS.
- Test on Android phones: touch, display cutouts, performance, and real audio latency.
- Continuous integration for macOS and Windows.
- Circle mode: slider ticks, repeats, and follow judgment; spinner judgment.
- Precise keyboard timestamps on Windows.
- An audio offset calibration screen, with offsets saved per output device.
- A chart editor built on the editor framework.
- More game modes: Drum, Highway, and Line.
- iOS with a Metal backend, and Linux.

## Contributing

Bug reports, questions, and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for how to build, test, and submit changes.

## License

Hyoshi Engine is released under the [MIT License](LICENSE).

The engine installs the Noto Sans JP fonts, which are under the SIL Open Font License 1.1. Third-party libraries keep their own licenses; see [docs/REFERENCES.md](docs/REFERENCES.md).

Built with [SDL3](https://github.com/libsdl-org/SDL), [volk](https://github.com/zeux/volk), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [Slang](https://github.com/shader-slang/slang), [miniaudio](https://github.com/mackron/miniaudio), [Dear ImGui](https://github.com/ocornut/imgui), [glm](https://github.com/g-truc/glm), [spdlog](https://github.com/gabime/spdlog), [yyjson](https://github.com/ibireme/yyjson), [stb](https://github.com/nothings/stb), and [doctest](https://github.com/doctest/doctest).
