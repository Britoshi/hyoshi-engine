# Handover

For whoever works on Hyoshi Engine next, human or AI. Read this first, then [README.md](../README.md) (modules, build, making a game, development hooks) and [DESIGN.md](DESIGN.md) (the plan, with "As built" notes where the code differs). Update this file at the end of every working session.

Last updated: 2026-09-28, trackpad fingers as input events (Mac) and the pointer lock. Before that, 2026-09-28, the Circle mode (M8, started early; DESIGN.md section 16.4), chart readers for the song library, and `SpriteBatch::KeepOrder`, and scenes that own textures surviving a scene switch. Before that, 2026-09-28, the editor framework's milestone 0 ([ADR 0002](decisions/0002-editor-framework.md)) and the first macOS build since mid M4. Before that, 2026-09-28, the repository's first commit. Before that, the engine and its first game shared one private repository; on 2026-09-28 they were split. The engine starts here with fresh history. The game keeps the old history in its own repository and uses this one as a git submodule.

## Where things stand

Hyoshi Engine is a mobile-first C++20 rhythm game engine (Vulkan through an RHI, SDL3, miniaudio), built milestone by milestone from DESIGN.md section 25. It's developed on macOS (Apple Silicon) and Windows 11 (MSVC); Android is the primary target and so far has run on the emulator only.

| Milestone | macOS | Windows | Android |
|---|---|---|---|
| M0 Foundation | Done | Done | Gradle glue and logcat done; emulator only |
| M1 Vulkan bring-up | Done, validation clean | Done, validation clean | Runs on the emulator (gfxstream), rotation with pre-rotation; no validation layer bundled; no phone yet |
| M2 2D renderer | Done (10,000 sprites at 120 Hz) | Done, with text and logos | Menus, text, and logos render on the emulator |
| M3 Audio and song clock | Done, stress-tested | Done, stress-tested | AAudio plays on the emulator; not stress-tested |
| M4 First playable (Mania) | The parts are here; the first game is built on them | Same | The first game plays on the emulator |
| M8 Circle mode (early) | Plays real osu!standard maps in a second game; sliders held, not followed | Not built | Not built |

**macOS, 2026-09-28: builds again, but validation isn't clean.** The engine builds on its own and as a game's subdirectory with Apple Clang, and the unit tests, `format-check`, and `tidy` pass. But every app now exits with a failure code: the validation layer reports 4 portability errors at startup, `vkCreateImageView(): swizzle is disabled for this device`, from the one-channel texture views in `engine/rhi/vulkan/VulkanResources.cpp:165` (swizzled to 1, 1, 1, R), which MoltenVK's portability subset doesn't allow by default. The engine at its first commit (`98937f9`) does the same, so the editor didn't cause it; when it began isn't known (the Mac hadn't built since mid M4). Not fixed yet. Two ways to fix it: turn on the portability subset's `imageViewFormatSwizzle` feature if MoltenVK offers it, or read the red channel in the shaders and drop the swizzle. The other checks (clock stress, surface stress) haven't been run on the Mac since mid M4.

**The Circle mode** (`modes/circle`, DESIGN.md section 16.4) plays osu!standard maps: import, slider paths, stacking, lazer's note lock, scoring, autoplay, and the playfield. A second game is built on it. What's missing for M8: slider ticks, repeats, tails, and follow judgment (a slider head is a Great and that's all), spinner judgment, and translucent slider bodies (they wait for render targets). The song library now takes a chart reader (`LibraryOptions`), so a game lists only its mode's charts; the default is Mania's, as before.

**The editor framework** (`engine/editor`, ADR 0002) is at milestone 0: a game's editor runs the game full-window with a menu bar and dockable ImGui panels over it. The first game's editor, with one panel, opens on the Mac (screenshot). Next is milestone 1: the chart and timeline panel, and playing from a point in a chart.

### Verified

By script (the development hooks, screenshots), by unit tests (82 cases), and through the games built on it, on Windows unless noted.

- The clock never goes backwards across device stalls and random seeks, and no scheduled clicks are late (`HYOSHI_SCENE=clock HYOSHI_STRESS_SEEK=1 HYOSHI_STRESS_AUDIO=1`, in the sample). No resyncs on macOS; on Windows a stress run sometimes has one (see "Known quirks").
- Autoplay scores 1,000,000 through the Mania judge, including across device stalls, and on real osu!mania maps (MP3), on Windows, macOS (before the UI work), and the Android emulator.
- The replay fixture in `tests/replays` matches its hand-computed result; live play and a replay of its actions give the same events.
- Vulkan validation is clean on Windows (layer 1.4.357): 120 surface resize and recreate cycles, 10,000 sprites, and every screen of the game. `spirv-val` passes the shaders.
- The UI kit, headless, with synthetic input: clicks, touch, lists, toggles, sliders, key repeats, wheel, drag, scroll limits. Text renders in Latin and Japanese.
- Logo distance fields: unit tests for edges, trimming, faint noise, and specks. The splash renders in landscape and portrait.
- The sample (`samples/metronome`) builds and runs; the engine builds on its own (`cmake --preset windows-debug`) and as a game's subdirectory.
- **macOS, 2026-09-28:** the engine builds on its own (with the sample) and as the game's subdirectory; 67 unit tests, `format-check`, and `tidy` pass. The game's autoplay scores 1,000,000 on the built-in drill. The game's editor opens with its menu bar and panel over the song select (screenshot), keeps it up through an autoplay run to the results, and shuts down cleanly. Validation isn't clean (above).
- **The Circle mode, macOS, 2026-09-28:** unit tests for the importer (old file formats, red lines resetting slider velocity), stacking, the judge (windows, misses, note lock, chords, holds), autoplay, determinism at three frame rates, and scoring. Headless, all 52 difficulties of 10 real osu!standard maps import without warnings and autoplay scores every note a Great. In the second game, autoplay plays whole maps through to the results with nothing but Greats (201, 669, and 1,658 judgments, AR 4 to 9.8, with and without a lead-in), and screenshots show the playfield: snaking and reversing sliders, a spinner, a deep stack, follow points, combo numbers. Switching scenes no longer frees a scene's textures while the frame still draws them (it crashed on the way to the results before). The first game, with its editor, builds without warnings on these engine changes, and its autoplay still scores 1,000,000 on the built-in drill through to the results.
- **Android emulator** (API 36, x86_64, Vulkan 1.3 through gfxstream, AAudio at 48 kHz): the game's APK builds for arm64-v8a and x86_64 through `platforms/android/hyoshi-app.gradle`; rendering, rotation, the safe area, immersive fullscreen, the back button (as Escape), and touch with OS timestamps (0.4 to 17 ms before the frame's pump, so SDL passes the event time through: the DESIGN.md section 12.3 question, answered for the emulator). Debug APKs bundle a game's `content/charts` and unpack it on first launch.

### Not verified

- **A real Android phone**: arm64 at all, real touch, cutouts, performance, AAudio's real latency, background/foreground cycles.
- **Audio sync by ear**: the M3 listening test hasn't been done.
- **FLAC and Ogg Vorbis decoding.** WAV and MP3 are exercised.
- **macOS**: validation (above), and the stress checks since mid M4.
- **The editor framework on Windows and Android.** Not built there after the change (2026-09-28): the `EditorLayer` changes to `Application`, the ImGui docking branch, and `hyoshi_add_editor` with MSVC; on Android, that `engine/editor` and `hyoshi_add_editor` stay out of the build, and that the docking branch still builds for the game. Build both before relying on them.
- **Trackpad events on a real trackpad** (2026-09-28): whether fingers arrive with the pointer locked, and how their timestamps compare with the keys'. The headless UI test only checks that menus ignore them.
- **The Circle mode on Windows and Android.** Not built there, nor the engine changes that came with it (2026-09-28: `SpriteBatch::KeepOrder`, `Application`'s ended scene, the song library's chart readers). And no one has played it by hand yet: real mouse and touch aim, and how its timing feels.
- **The editor by hand**: docking, the View menu, File > Quit, and typing into a panel while a game runs (the ImGui windows should take the keys). Which panels are open isn't saved between runs; where they are is.

## Map of the code

| Path | What |
|---|---|
| `engine/core` | `Result<T>`, logging, handles, `Time.h` (integer µs and frame math, `ToMilliseconds`), `Env.h` (environment helpers), `SpscQueue`, `SeqLock`, `JobSystem` |
| `engine/platform` | SDL3: window, events into `InputQueue` (keys with repeats marked, wheel, touch, pointer, Mac trackpad fingers), pointer lock, lifecycle, host clock, base, user data, asset, and shared storage paths, safe area, orientation, window icon, `LoadFile` / `SaveFile` (atomic) / `MakeDirectory` |
| `engine/input` | `InputEvent`, `InputQueue` (sorted by host time) |
| `engine/rhi` | `IRenderDevice` and the Vulkan backend (volk, VMA, MoltenVK on macOS) |
| `engine/renderer` | `Camera2D` (short side 1080), `SpriteBatch` (sprites can be distance fields; `WithAlpha`; `KeepOrder` layers draw in submission order), `TextRenderer` (runtime SDF glyphs from TTF/OTF, atlas pages), `Image` (PNG/JPEG, distance fields from alpha, textures) |
| `engine/debug` | Dear ImGui overlay (positions saved in `imgui.ini` in the user data directory; the editor's in `editor-imgui.ini`, with docking on) |
| `engine/audio` | `IAudioBackend`, `Mixer` (runs in the callback), miniaudio backend and decoder, `Synth` (clicks, metronome track) |
| `engine/rhythm` | `SongClock`, chart format (`ChartFile`), `ScrollMap`, `Judgment`, `ScoreSystem`, `OffsetCalibration` |
| `engine/songs` | `Charts` (`ChartSource`, `GetChartKey`, the built-in drill, loading `.osu` / `.rchart.json` as Mania, `.osu` as Circle), `SongLibrary` (scans song folders on a worker with a chart reader from `LibraryOptions`; `UnpackBundledSongs`), `SongPlayer` (music and the song clock) |
| `engine/ui` | `Ui` (immediate-mode widgets, layout rectangles, colors, layers), `Logo` (images as distance fields, drawn to fit) |
| `engine/app` | `Application` (main loop, scene switching with a fade, lifecycle, development hooks, exit checks), `Game` (the interface a game implements), `EditorLayer` (the editor's hook into the main loop), `Scene`, `ClockDemo` (`HYOSHI_SCENE=clock`), the splash |
| `engine/editor` | Desktop only. `Editor` (the interface a game's editor implements), `Panel`, `EditorServices`, `Run`; the menu bar and dock space (ADR 0002) |
| `modes/mania` | `ManiaChart`, `ManiaJudge` (+ autoplay), `ManiaPlayfield` (drawing) |
| `modes/circle` | `CircleChart` (paths, stacking, combo numbers: `FinishCircleChart`), `CircleJudge` (+ autoplay), `CircleScore`, `CirclePlayfield` (drawing, `CircleLayout`) |
| `tools/osu-import` | `ImportOsuMania`, `ImportOsuStandard` (`OsuText.h` holds the shared parsing), and the osu!mania to `.rchart.json` CLI |
| `tools/asset-cooker` | `hyoshi-asset-cooker icons`: a game's icons from its logo |
| `samples/metronome` | The smallest game: splash, then the clock demo |
| `platforms/windows` | The UTF-8 code page manifest every executable embeds |
| `platforms/android` | SDL's Java glue (`org/libsdl/app`, from SDL 3.4.16), `HyoshiActivity`, `hyoshi-app.gradle` (a game's Android build) |
| `cmake/` | CPM and pinned dependencies (ImGui is the docking branch), warnings, shaders, `hyoshi_add_app` and `hyoshi_add_editor` (HyoshiApp.cmake), `hyoshi_add_code_checks` (Tooling.cmake) |
| `docs/decisions` | Architecture decision records: 0002, the editor framework (0001, the stack choices, isn't written yet) |
| `tests/unit`, `tests/replays` | doctest unit tests; replay fixtures |

### How a game plugs in

A game implements `hyoshi::app::Game` and passes it to `hyoshi::app::Application` with an `AppConfig` (name, window icon, whether to show the splash). `Application::Run` brings up the window, device, and audio, then calls `Game::Initialize` with the `AppServices` (platform, device, audio, jobs), shows the splash, and asks `Game::CreateFirstScene`. Each frame it calls `Game::Update`, then the scene's `RunFrame`; a scene moves on with `SwitchTo(nextScene)`, and the switch happens after the frame, with a fade. On backgrounding and at exit it calls `Game::Save`. The engine's assets install under `engine/` in the app's assets (`hyoshi_add_app`), a game's under their own folder names.

A game's editor (ADR 0002) is a second executable from the same game code, built with `hyoshi_add_editor`, whose `main` calls `editor::Run(config, game, editor)`. `Run` gives the `Application` an `EditorLayer` (and no splash). With one, the `Application` keeps ImGui on with docking and `editor-imgui.ini`, lets the ImGui windows capture keys and the pointer while they want them, sets `AppServices::IsEditor`, calls the layer's `Initialize` after `Game::Initialize` (so the game's services exist), its `Build` every frame before the debug overlay's windows, and its `Shutdown` before `Game::Shutdown`. The layer draws the menu bar (File, View), a dock space over the window whose empty center passes input through to the game, and the game's panels. `AppConfig::WindowTitle` lets the editor's title differ while it shares the game's user data folder (`Name`).

### How time flows

1. The audio callback publishes an `AudioClockSnapshot` (music frame, host time, seek generation) through a seqlock.
2. `SongPlayer::Update` feeds it to `SongClock`, which keeps a filtered mapping `songTime = hostUs + mappingOffset` anchored to the music cursor.
3. Input events carry OS timestamps in host time. A game converts them with `HostTimeToSongTime` and passes them to the judge (`ManiaJudge::ProcessAction`).
4. Each frame, the judge advances to 20 ms behind the current song time, to judge misses and hold ends.
5. Sounds that belong to notes are scheduled on music frames with `PlayAtMusicFrame`, so they follow pauses and seeks.

### How a frame flows

`Application::Run`: pump events, take the input queue, F1 toggles the overlay, `SpriteBatch::Begin`, `Game::Update`, then `RunScene`: `Ui::BeginFrame` reads the input, and the scene's `RunFrame` updates and draws through the UI in one call. A scene's `SwitchTo` takes effect after the scene's `RunFrame`: `EndScene` logs the old one's summary and keeps its failed check and backward steps for the exit checks, and the new scene runs from the next frame. The old scene is destroyed only after the frame is recorded, because its sprites, and so its textures, are still in the frame's batch (a scene that destroyed its texture in its destructor crashed the switch until 2026-09-28). `RenderFrame` then adds the stress sprites and the fade, flushes new glyphs (`TextRenderer::Flush`), and ends the sprite batch.

## Invariants: don't break these

- **Gameplay time is integer microseconds** (`SongTimeUs`, `HostTimeNs`). No floats in time comparisons.
- **Song time never goes backwards** except across a seek. `SongPlayer` counts backward steps, and a debug run exits with a failure if any occur, including in scenes that already ended.
- **The judge is deterministic.** It depends only on the chart and the timestamped actions. Live play and a replay of the recorded actions must give the same events (there's a unit test).
- **Offset sign.** `songTime = audibleMusicTime − outputLatency − userAudioOffset − chartOffset`. A positive user offset makes notes later and fixes hitting late. Corrections add the median error to the offset.
- **`BuildManiaChart` clears `Info.Notes`.** `ManiaChart::Notes` is the only copy of the notes; `ManiaNote` carries the sample fields.
- **The engine never writes into song folders.** It only reads them. The one exception is the `dev-songs/` cache it unpacks from a debug APK (`UnpackBundledSongs`), in the app's private data.
- **Release APKs never carry a game's local maps.** `bundleDevSongs` in `hyoshi-app.gradle` is wired to debug variants only; `assembleRelease --dry-run` doesn't list it.
- **Engine code never depends on a game.** Games use the engine's public headers and CMake functions; nothing under `engine/`, `modes/`, or `tools/` names a game.

## Conventions

- Build and test with the `macos-debug` presets on macOS and `windows-debug` on Windows. `format`, `format-check`, and `tidy` are build targets. Tidy enforces naming: PascalCase types, functions, and public members (test fixtures included); camelCase locals and private members; UPPER_CASE constants.
- Commit after each working step. Message style: an imperative subject line, then a body of `- area: what and why` bullets, and a "Checked:" paragraph saying what was verified.
- When the code departs from DESIGN.md, add an "As built (Mx)" note next to the original plan. Don't rewrite the plan.
- Docs are plain and specific: what was built, and what was and wasn't verified.
- UI: draw backgrounds and text on different layers (the sprite batch groups a layer's sprites by texture), and give list rows ids that don't include their position (`Ui::WasClicked`).

## How checks are run

```sh
S=/tmp/hyoshi-check   # any scratch folder
# Clock stress in the sample: expect 0 resyncs (0 or 1 on Windows), 0 backward steps, 0 late voices
HYOSHI_SCENE=clock HYOSHI_STRESS_SEEK=1 HYOSHI_STRESS_AUDIO=1 HYOSHI_EXIT_AFTER_MS=12000 \
  ./build/macos-debug/samples/metronome/HyoshiMetronome
# Surface stress and a screenshot
HYOSHI_STRESS_SURFACE=1 HYOSHI_EXIT_AFTER_MS=12000 HYOSHI_SCREENSHOT=$S/shot.png HYOSHI_SCREENSHOT_MS=5000 \
  ./build/macos-debug/samples/metronome/HyoshiMetronome
```

Gameplay checks (autoplay at the maximum score, offset signs, menus) run in a game, and are documented with it. On Windows, environment variables set in PowerShell (`$env:HYOSHI_SCENE = "clock"`) stay set for the rest of that process; clear one with `$env:HYOSHI_SCENE = $null`.

**Windows.** cl and the Windows SDK need a developer environment. In a PowerShell script:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset windows-debug; cmake --build --preset windows-debug; ctest --preset windows-debug
```

Posting key messages (`WM_KEYDOWN`/`WM_KEYUP`) to the window drives an app without focus; posted mouse messages don't work reliably, because SDL tracks the real cursor. Test widgets with the headless UI tests instead.

**Android.** A game's Gradle build does the native build (README, "Android"). For a native compile error, the fastest loop is Ninja in the configured build folder, which shows every error at once: `ninja -C <game>/android/app/.cxx/Debug/<hash>/arm64-v8a -k 0 <target>` (Ninja is in the SDK's `cmake/4.1.2/bin`). With the emulator: `adb exec-out screencap -p > shot.png`, `adb shell input tap X Y` (touch events with real timestamps), `adb shell input keyevent KEYCODE_BACK`, and `adb logcat -d --pid=$(adb shell pidof <app id>)` (the engine's lines are tagged `Hyoshi`). Environment variables don't reach Android apps, so there are no scripted runs there yet.

## Gotchas

- **Sentinel overflow.** Twice now, `INT64_MIN` as a "never happened" time overflowed in `now − start`, and something showed that shouldn't have. Use `std::optional` or check for the sentinel first.
- **Uncapped frame rate.** With the display asleep, presentation isn't throttled and an app runs at about 3,000 FPS. Frame-based screenshots then land too early, so use `HYOSHI_SCREENSHOT_MS`.
- **Stale clangd.** clangd often reports stale "file not found" errors for new files. Trust the build.
- **Trackpads.** Mac trackpad fingers arrive as `TrackpadDown/Move/Up`, not touches: their X and Y are positions on the pad (from NSTouch's normalized position, through SDL's `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY`, set before `SDL_Init`), and the OS moves the pointer from the same fingers. Only touchscreens produce touch input. AppKit sends a trackpad's touches to the view under the pointer, so a game aiming with them locks the pointer in its window (`Platform::SetPointerLocked`). Windows touchpads only move the pointer.
- **stb_truetype's SDF and cubic curves.** `stbtt_GetGlyphSDF` ignores cubic segments, so CFF fonts (Noto Sans JP's `.otf`) lost their round letters. `TextRenderer` computes the field itself from the flattened outline; don't switch back.
- **Sprite order within a layer.** `SpriteBatch` sorts a layer's sprites by texture, so a rounded panel (circle texture), a plain rectangle (white texture), and text (atlas pages) on one layer draw in texture order, not call order. Stack things on different layers, or call `KeepOrder(layer)` for that frame when many things must overlap in order (the circle playfield's notes and their numbers); each texture change in such a layer is a draw call.
- **Layers run out.** The app's fade is at `ui::layers::FADE` (60) in the same sprite batch, and the HUD at 20, so a playfield lives in the layers between. The circle playfield takes five.
- **MSVC vs clang.** Each accepts things the other rejects: libc++ includes some standard headers transitively that MSVC's library doesn't (include what you use), and clang rejects a forward declaration that collides with a using-declaration where MSVC compiled it (clang-tidy caught it). Build or tidy with both.
- **Windows: CRLF checkout.** With `core.autocrlf=true`, the working tree has CRLF line endings and commits are normalized to LF. The build, tests, and format-check pass either way.
- **Windows: "'vswhere.exe' is not recognized".** The developer environment script prints this on every entry. It's harmless.
- **Windows: key timestamps are coarse.** SDL 3.4 stamps Windows key and mouse messages from `GetMessageTime`, which moves in 15 to 16 ms steps, as coarse as the ±16 ms Perfect window. The likely fix is raw input (`WM_INPUT`) on a dedicated thread, stamped with `QueryPerformanceCounter` (DESIGN.md section 12.3).
- **Android: bionic's `PAGE_SIZE` macro.** Bionic defines `PAGE_SIZE` in `<bits/page_size.h>`, which the standard headers pull in, so a constant by that name fails to compile (it was `TextRenderer`'s; now `ATLAS_PAGE_SIZE`). Avoid names that are common C macros.
- **Android: no floating-point `std::from_chars`.** The NDK's libc++ (LLVM 18) only has the integer overloads. The importers' `ParseNumber` (`OsuText.h`) falls back to `strtod` for libc++ before 20; nothing may call `setlocale`, or that fallback reads commas.
- **Android: `sdkmanager` from PowerShell.** It's a `.bat`, and cmd splits `cmake;4.1.2` at the semicolon: quote it inside a `cmd /c "..."` string. It prints that it's deprecated in favour of `android sdk`; it still works.
- **Android: `local.properties`.** Use forward slashes in `sdk.dir`; backslashes are escapes in a properties file.
- **Android: busy folders.** On Windows, the Gradle daemon (`gradlew --stop`) and the adb server (`adb kill-server`) keep the folder they started in open, so it can't be renamed or moved.
- **Missing validation layer.** The debug messenger only runs when the validation layer loaded; the loader's own warnings would otherwise fail debug runs.

## Known quirks

- **After an audio device restart, the clock starts up to about 35 ms behind.** When WASAPI restarts, miniaudio fills the whole device buffer at once: 3 or 4 callbacks within about 6 ms, advancing the music cursor 30 to 40 ms. `SongClock` re-anchors on the first of them, then sees an error of +29 to +35 ms. Above 30 ms it snaps and counts a resync. Below that, it corrects at 1 ms per frame, so for about 30 frames song time is late by up to 29 ms. CoreAudio calls back in real time and doesn't do this; AAudio might. A fix: after a stall, keep re-anchoring on each new snapshot, without counting resyncs, until one arrives in real time.
- **An offset change eases in over about 0.2 s** (the clock's filter corrects 10% per frame). Jumps of 30 ms or more re-anchor at once.
- **If AAudio refuses the stream, the app is silent.** Right after an emulator boot, AAudio once returned `AAUDIO_ERROR_ILLEGAL_ARGUMENT`, and miniaudio fell back to its Null backend for the whole session; restarting the app fixed it. Retrying, or falling back to OpenSL ES, would be more robust.
- **Logos are processed at startup.** The splash's distance fields are computed when the app starts, so unoptimized Android builds show the system splash for about a second first. Cooking them offline would remove that.

## Next steps, roughly in order

1. macOS: fix the validation errors (above), then run the stress checks there.
2. A real Android phone: M1–M4 on the device (touch, cutouts, 20 background/foreground cycles, rotation), AAudio timestamps for the true output latency, and bundling the validation layer in debug builds.
3. The clock after a device restart (above). It changes `SongClock`, so it affects every platform.
4. GPU note positioning (DESIGN.md section 10.5). Notes are placed on the CPU now.
5. Offsets per output route (DESIGN.md section 11.5), and a calibration scene (M5).
6. CI (GitHub Actions) running the unit tests and replay fixtures on macOS and Windows, Tracy, ADR 0001.
7. Windows, when it matters for play: precise key timestamps.
8. Circle (M8): slider ticks, repeats, tails, and follow judgment, with the follow radius and pointer tracking (the judge already records Moves); spinner judgment; then build the mode on Windows and Android.
9. The editor framework's milestone 1 (ADR 0002): the chart and timeline panel (DESIGN.md section 19.4), undo, file dialogs, and a play-mode hook so the game's gameplay starts from a point in the chart. Then milestone 2: RHI render targets and a camera per viewport, so the game draws inside a panel. Render targets also let the circle playfield draw translucent slider bodies.
