# Development

Notes for working on the engine: debug options, stress tests, platform-specific tips, and known issues. Start with the [README](../README.md) for building, and [ARCHITECTURE.md](ARCHITECTURE.md) for how the code is organized.

## Debug builds

Debug builds enable the Vulkan validation layer. An app exits with a failure code if validation reported anything, if song time went backwards, or if a scene reported a failed check, so scripts can run an app and check its exit code.

## Environment variables

These work in any app built on the engine, for scripted runs and tests:

| Variable | Effect |
|---|---|
| `HYOSHI_EXIT_AFTER_MS=3000` | Quit after 3 seconds |
| `HYOSHI_SCREENSHOT=out.png` | Save frame 30 as a PNG (`HYOSHI_SCREENSHOT_FRAME=400` for another frame) |
| `HYOSHI_SCREENSHOT_MS=5000` | Take the screenshot this long after startup instead (see [uncapped frame rate](#platform-notes)) |
| `HYOSHI_WINDOW_SIZE=720x1280` | Open the window at this size, for example portrait |
| `HYOSHI_PRESENT_MODE=immediate` | Present with `fifo`, `mailbox`, or `immediate` (tearing) where the surface offers it, instead of the game's choice (FIFO, or immediate with `AppConfig::LowLatency`), to compare how they feel |
| `HYOSHI_FRAME_CAP=141` | Cap desktop frames at this many a second instead of four times the refresh rate (or none, under FIFO); for example just under a variable refresh display's maximum |
| `HYOSHI_DEBUG_UI=1` | Show the debug overlay from the start (F1 toggles it) |
| `HYOSHI_SCENE=clock` | Run the clock demo instead of the game |
| `HYOSHI_STRESS_SURFACE=1` | Every 10 frames, alternately resize the window and destroy and recreate the surface |
| `HYOSHI_STRESS_AUDIO=1` | Stop the audio device for the last 200 ms of every 3 seconds |
| `HYOSHI_STRESS_SEEK=1` | Clock demo: seek to a random position every 1.5 seconds |
| `HYOSHI_STRESS_REOPEN=1` | Close the audio output and open it again every 2 seconds (`IAudioBackend::Reopen`, a game's Restart audio), the music playing on |
| `HYOSHI_SPRITES=10000` | Add bouncing sprites, for renderer stress tests |
| `HYOSHI_MUSIC=song.ogg`, `HYOSHI_BPM=128`, `HYOSHI_FIRST_BEAT_MS=350` | Clock demo: stream this file instead of the generated metronome track, at this tempo |

On Windows, variables set in PowerShell (`$env:HYOSHI_SCENE = "clock"`) stay set for the rest of the session; clear one with `$env:HYOSHI_SCENE = $null`.

## Checks

Before sending a change, run the unit tests and the code checks:

```sh
ctest --preset macos-debug
cmake --build --preset macos-debug --target format-check
cmake --build --preset macos-debug --target tidy
```

`--target format` reformats the code in place.

For changes to timing, audio, or rendering, also run the stress tests in the sample:

```sh
# Clock stress: expect 0 resyncs (0 or 1 on Windows), 0 backward steps, 0 late voices, and 0 music underruns
HYOSHI_SCENE=clock HYOSHI_STRESS_SEEK=1 HYOSHI_STRESS_AUDIO=1 HYOSHI_EXIT_AFTER_MS=12000 \
  ./build/macos-debug/samples/metronome/HyoshiMetronome

# The same with streamed music: any song file, with HYOSHI_BPM for the clicks
HYOSHI_SCENE=clock HYOSHI_MUSIC=song.mp3 HYOSHI_BPM=128 HYOSHI_STRESS_SEEK=1 HYOSHI_STRESS_AUDIO=1 \
  HYOSHI_EXIT_AFTER_MS=12000 ./build/macos-debug/samples/metronome/HyoshiMetronome

# Surface stress, with a screenshot
HYOSHI_STRESS_SURFACE=1 HYOSHI_EXIT_AFTER_MS=12000 HYOSHI_SCREENSHOT=shot.png HYOSHI_SCREENSHOT_MS=5000 \
  ./build/macos-debug/samples/metronome/HyoshiMetronome
```

Gameplay checks, such as autoplay reaching the maximum score, run in a game built on the engine.

## Windows

The compiler and the Windows SDK need a developer environment. In a PowerShell script:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset windows-debug; cmake --build --preset windows-debug; ctest --preset windows-debug
```

The developer environment prints "'vswhere.exe' is not recognized" on every start; it's harmless.

To drive an app without focus, post key messages (`WM_KEYDOWN`, `WM_KEYUP`) to its window. Posted mouse messages don't work reliably, because SDL tracks the real cursor; test widgets with the headless UI tests instead.

## Android

A game's Gradle build runs the native build (see the README). For native compile errors, it's faster to run Ninja in the configured build folder, which shows every error at once:

```sh
ninja -C <game>/android/app/.cxx/Debug/<hash>/arm64-v8a -k 0 <target>
```

Ninja is in the SDK's `cmake/4.1.2/bin`. Useful commands with a device or emulator:

```sh
adb exec-out screencap -p > shot.png
adb shell input tap X Y                         # touch events with real timestamps
adb shell input keyevent KEYCODE_BACK
adb logcat -d --pid=$(adb shell pidof <app id>) # the engine's lines are tagged Hyoshi
```

Environment variables don't reach Android apps, so there are no scripted runs on Android yet.

## Platform notes

- **Sentinel values.** Using `INT64_MIN` as a "never happened" time has overflowed in `now − start` more than once. Use `std::optional`, or check for the sentinel first.
- **Uncapped frame rate.** When the display is asleep, presentation isn't throttled and an app runs at thousands of frames per second, so frame-based screenshots come too early. Use `HYOSHI_SCREENSHOT_MS`.
- **clangd.** clangd often reports "file not found" for newly added files. Trust the build.
- **MSVC and Clang.** Each accepts code the other rejects. libc++ includes some standard headers transitively where MSVC's library doesn't, so include what you use. Clang rejects a forward declaration that collides with a using-declaration where MSVC accepts it. Build or run clang-tidy with both when you can.
- **Sprite order within a layer.** `SpriteBatch` sorts each layer's sprites by texture, so sprites with different textures on the same layer draw in texture order, not call order. Put overlapping things on different layers, or call `KeepOrder(layer)` for that frame. Each texture change in a `KeepOrder` layer costs a draw call.
- **Layers.** The app's fade uses `ui::layers::FADE` (60) in the same sprite batch, and the HUD uses 20, so a playfield must fit in the layers between. The circle playfield uses five.
- **MP3 seeks.** miniaudio's MP3 seeks land up to two frames (2,304 samples) late, or the encoder delay early, from where decoding from the start puts the same frame, varying through a file. So MP3s decode through the engine's own backend (`audio/miniaudio/Mp3Backend.cpp`, DESIGN.md section 11.2), whose seeks are exact. Don't hand MP3s back to miniaudio's own decoding, or change the backend's seek, without checking seeks against a full decode, as the unit test does on `tests/audio`'s MP3s. miniaudio's implementation is compiled in `Mp3Backend.cpp`, since dr_mp3 is declared only inside it.
- **Distance-field text.** `stbtt_GetGlyphSDF` ignores cubic curves, which breaks CFF fonts such as Noto Sans JP's `.otf`. `TextRenderer` computes the distance field itself from the flattened outline; don't switch back.
- **Mac trackpads.** Mac trackpad fingers arrive as `TrackpadDown`, `TrackpadMove`, and `TrackpadUp` events, not touches. Their positions are on the pad (from NSTouch's normalized position, through SDL's `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY`, set before `SDL_Init`), and the OS still moves the pointer from the same fingers. AppKit sends trackpad touches to the view under the pointer, so a game that aims with the trackpad locks the pointer in its window (`Platform::SetPointerLocked`). Windows touchpads only move the pointer.
- **Windows mouse button timestamps are coarse** unless the pointer is locked. SDL stamps Windows window messages with `GetMessageTime`, which moves in steps of 15 to 16 ms. Keys come as raw input instead (`SDL_HINT_WINDOWS_RAW_KEYBOARD`, set in `Platform::Initialize`), as does the mouse while locked (relative mode), stamped as they arrive ([DESIGN.md section 12.3](DESIGN.md#123-timestamp-accuracy-verify-early)).
- **Windows line endings.** With `core.autocrlf=true`, the working tree has CRLF line endings and commits are normalized to LF. The build, tests, and format check pass either way.
- **Android: `int64_t` is `long`.** So `SongTimeUs` and `std::llround`'s `long long` are different types there, and `std::max(time, std::llround(x))` doesn't compile with the NDK though MSVC takes it; name the type (`std::max<SongTimeUs>`).
- **Android: 16 KB pages.** Android 15 devices can use 16 KB memory pages, and Google Play requires native code aligned for them. NDK r27 aligns for 4 KB unless asked, so `hyoshi-app.gradle` passes `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (r28 does it by default); without it, a debuggable APK on such a phone shows an "Android App Compatibility" warning that `libmain.so`'s LOAD segment isn't aligned. Check with `llvm-readelf -l libmain.so` (each LOAD's alignment 0x4000).
- **Android: `PAGE_SIZE`.** Bionic defines a `PAGE_SIZE` macro that the standard headers pull in, so a constant with that name fails to compile. Avoid names that are common C macros.
- **Android: no floating-point `std::from_chars`.** The NDK's libc++ only has the integer overloads. The importers' `ParseNumber` (`OsuText.h`) falls back to `strtod` on libc++ before version 20, so nothing may call `setlocale`, or the fallback would read commas as decimal points.
- **Android: `sdkmanager` from PowerShell.** It's a `.bat` file, and cmd splits `cmake;4.1.2` at the semicolon; quote it inside a `cmd /c "..."` string.
- **Android: `local.properties`.** Use forward slashes in `sdk.dir`; backslashes are escape characters in a properties file.
- **Android on Windows: locked folders.** The Gradle daemon (`gradlew --stop`) and the adb server (`adb kill-server`) keep the folder they started in open, so it can't be renamed or moved while they run.
- **Web: WebGPU, else WebGL 2.** The page asks for a WebGPU device before the module starts (`platforms/web/index.html.in`); without one (an older browser, a car's), the engine draws with WebGL 2 (`engine/rhi/webgl`). `?renderer=webgl` (or `HYOSHI_RENDERER=webgl`) forces WebGL, to check it on a browser that has WebGPU. Its shaders are hand-written GLSL ES copies of the Slang ones (`SpriteGles.h`, `DebugUiGles.h`): change both together.
- **Web: Asyncify, not JSPI.** The main loop gives the browser control once a frame (`WaitForAnimationFrame`, `EM_ASYNC_JS`), which older browsers can only do through Asyncify. It's linked with `ASYNCIFY_IGNORE_INDIRECT`, so only direct calls from `main` to that wait are instrumented, plus `SDL_CallMainFunction` (`ASYNCIFY_ADD`), which calls the app's main through a pointer: without it, resuming crashes with "memory access out of bounds". Keep the path direct, and nothing else may suspend: SDL's own delays are told not to (`SDL_HINT_EMSCRIPTEN_ASYNCIFY`), and `Platform::WaitForEvents` returns at once on the web.
- **Web builds on Windows.** Run them from Visual Studio's environment (for Ninja) after `emsdk_env.bat`, with `HOME` set (`set HOME=%USERPROFILE%`): the presets put the dependency cache under `$env{HOME}`, which cmd doesn't set, and the compiler can't find `/.cache/CPM`.
- **Missing validation layer.** The debug messenger runs only when the validation layer is loaded; otherwise the loader's own warnings would fail debug runs.

## Known issues

- **macOS: Vulkan validation errors.** At startup, the validation layer reports `vkCreateImageView(): swizzle is disabled for this device` four times, from the single-channel texture views in `engine/rhi/vulkan/VulkanResources.cpp`, which are swizzled to (1, 1, 1, R). MoltenVK's portability subset doesn't allow this by default, so every debug run on macOS exits with a failure code. Possible fixes: enable the portability subset's `imageViewFormatSwizzle` feature if MoltenVK supports it, or read the red channel in the shaders and remove the swizzle.
- **The clock starts up to 35 ms late after an audio device restart.** When WASAPI restarts, miniaudio fills the whole device buffer at once (3 or 4 callbacks within about 6 ms), moving the music position forward 30 to 40 ms. `SongClock` re-anchors on the first callback and then sees an error of +29 to +35 ms. Above 30 ms it snaps and counts a resync; below, it corrects by 1 ms per frame, so song time is late for about 30 frames. CoreAudio doesn't do this; AAudio might. A possible fix: after a stall, re-anchor on each new snapshot, without counting resyncs, until one arrives in real time.
- **Offset changes ease in over about 0.2 seconds,** because the clock's filter corrects 10% per frame. Jumps of 30 ms or more re-anchor immediately.
- **Android: silent if AAudio refuses the stream.** Right after an emulator boot, AAudio once returned `AAUDIO_ERROR_ILLEGAL_ARGUMENT`, and miniaudio fell back to its null backend for the whole session. Restarting the app fixed it. Retrying, or falling back to OpenSL ES, would be more reliable.
- **Logos are processed at startup.** The splash's wordmark is made a distance field when the app starts, so unoptimized Android builds show the system splash for about a second first. Processing them at build time would fix this.

## Not yet tested

- A real Android phone: arm64, real touch, display cutouts, performance, AAudio's real latency, and background and foreground cycles.
- Audio sync by ear.
- FLAC decoding. WAV, MP3, and Ogg Vorbis are tested.
- Streamed music on Android, and how far ahead a slow phone needs to decode.
- The stress tests on macOS.
- The editor framework, Circle mode, and trackpad input on Android.
- Whether Mac trackpad fingers keep arriving with the pointer locked, and how their timestamps compare with key presses.
- The editor by hand: docking, the View menu, File > Quit, and typing into a panel while a game runs. Which panels are open isn't saved between runs; their positions are.
