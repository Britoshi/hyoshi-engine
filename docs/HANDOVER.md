# Handover

For whoever works on Hyoshi Engine next, human or AI. Read this first, then [README.md](../README.md) (modules, build, making a game, development hooks) and [DESIGN.md](DESIGN.md) (the plan, with "As built" notes where the code differs). Update this file at the end of every working session.

Last updated: 2026-09-28, the repository's first commit. Before that, the engine and its first game shared one private repository; on 2026-09-28 they were split. The engine starts here with fresh history. The game keeps the old history in its own repository and uses this one as a git submodule.

## Where things stand

Hyoshi Engine is a mobile-first C++20 rhythm game engine (Vulkan through an RHI, SDL3, miniaudio), built milestone by milestone from DESIGN.md section 25. It's developed on macOS (Apple Silicon) and Windows 11 (MSVC); Android is the primary target and so far has run on the emulator only.

| Milestone | macOS | Windows | Android |
|---|---|---|---|
| M0 Foundation | Done | Done | Gradle glue and logcat done; emulator only |
| M1 Vulkan bring-up | Done, validation clean | Done, validation clean | Runs on the emulator (gfxstream), rotation with pre-rotation; no validation layer bundled; no phone yet |
| M2 2D renderer | Done (10,000 sprites at 120 Hz) | Done, with text and logos | Menus, text, and logos render on the emulator |
| M3 Audio and song clock | Done, stress-tested | Done, stress-tested | AAudio plays on the emulator; not stress-tested |
| M4 First playable (Mania) | The parts are here; the first game is built on them | Same | The first game plays on the emulator |

**Not built on macOS since the Windows work began** (mid M4): the Windows changes, the text renderer, the UI kit, images, the app framework, and the split. Build and run the checks there first; watch for libc++ missing `std::format`, and for anything MSVC accepted that Clang won't.

### Verified

By script (the development hooks, screenshots), by unit tests (67 cases), and through the first game built on it, on Windows unless noted.

- The clock never goes backwards across device stalls and random seeks, and no scheduled clicks are late (`HYOSHI_SCENE=clock HYOSHI_STRESS_SEEK=1 HYOSHI_STRESS_AUDIO=1`, in the sample). No resyncs on macOS; on Windows a stress run sometimes has one (see "Known quirks").
- Autoplay scores 1,000,000 through the Mania judge, including across device stalls, and on real osu!mania maps (MP3), on Windows, macOS (before the UI work), and the Android emulator.
- The replay fixture in `tests/replays` matches its hand-computed result; live play and a replay of its actions give the same events.
- Vulkan validation is clean on Windows (layer 1.4.357): 120 surface resize and recreate cycles, 10,000 sprites, and every screen of the game. `spirv-val` passes the shaders.
- The UI kit, headless, with synthetic input: clicks, touch, lists, toggles, sliders, key repeats, wheel, drag, scroll limits. Text renders in Latin and Japanese.
- Logo distance fields: unit tests for edges, trimming, faint noise, and specks. The splash renders in landscape and portrait.
- The sample (`samples/metronome`) builds and runs; the engine builds on its own (`cmake --preset windows-debug`) and as a game's subdirectory.
- **Android emulator** (API 36, x86_64, Vulkan 1.3 through gfxstream, AAudio at 48 kHz): the game's APK builds for arm64-v8a and x86_64 through `platforms/android/hyoshi-app.gradle`; rendering, rotation, the safe area, immersive fullscreen, the back button (as Escape), and touch with OS timestamps (0.4 to 17 ms before the frame's pump, so SDL passes the event time through: the DESIGN.md section 12.3 question, answered for the emulator). Debug APKs bundle a game's `content/charts` and unpack it on first launch.

### Not verified

- **A real Android phone**: arm64 at all, real touch, cutouts, performance, AAudio's real latency, background/foreground cycles.
- **Audio sync by ear**: the M3 listening test hasn't been done.
- **FLAC and Ogg Vorbis decoding.** WAV and MP3 are exercised.
- **macOS since mid M4** (above).

## Map of the code

| Path | What |
|---|---|
| `engine/core` | `Result<T>`, logging, handles, `Time.h` (integer µs and frame math, `ToMilliseconds`), `Env.h` (environment helpers), `SpscQueue`, `SeqLock`, `JobSystem` |
| `engine/platform` | SDL3: window, events into `InputQueue` (keys with repeats marked, wheel, touch, pointer), lifecycle, host clock, base, user data, asset, and shared storage paths, safe area, orientation, window icon, `LoadFile` / `SaveFile` (atomic) / `MakeDirectory` |
| `engine/input` | `InputEvent`, `InputQueue` (sorted by host time) |
| `engine/rhi` | `IRenderDevice` and the Vulkan backend (volk, VMA, MoltenVK on macOS) |
| `engine/renderer` | `Camera2D` (short side 1080), `SpriteBatch` (sprites can be distance fields; `WithAlpha`), `TextRenderer` (runtime SDF glyphs from TTF/OTF, atlas pages), `Image` (PNG/JPEG, distance fields from alpha, textures) |
| `engine/debug` | Dear ImGui overlay (positions saved in `imgui.ini` in the user data directory) |
| `engine/audio` | `IAudioBackend`, `Mixer` (runs in the callback), miniaudio backend and decoder, `Synth` (clicks, metronome track) |
| `engine/rhythm` | `SongClock`, chart format (`ChartFile`), `ScrollMap`, `Judgment`, `ScoreSystem`, `OffsetCalibration` |
| `engine/songs` | `Charts` (`ChartSource`, `GetChartKey`, the built-in drill, loading `.osu` / `.rchart.json`), `SongLibrary` (scans song folders on a worker; `UnpackBundledSongs`), `SongPlayer` (music and the song clock) |
| `engine/ui` | `Ui` (immediate-mode widgets, layout rectangles, colors, layers), `Logo` (images as distance fields, drawn to fit) |
| `engine/app` | `Application` (main loop, scene switching with a fade, lifecycle, development hooks, exit checks), `Game` (the interface a game implements), `Scene`, `ClockDemo` (`HYOSHI_SCENE=clock`), the splash |
| `modes/mania` | `ManiaChart`, `ManiaJudge` (+ autoplay), `ManiaPlayfield` (drawing) |
| `tools/osu-import` | `.osu` to `.rchart.json` converter (library + CLI) |
| `tools/asset-cooker` | `hyoshi-asset-cooker icons`: a game's icons from its logo |
| `samples/metronome` | The smallest game: splash, then the clock demo |
| `platforms/windows` | The UTF-8 code page manifest every executable embeds |
| `platforms/android` | SDL's Java glue (`org/libsdl/app`, from SDL 3.4.16), `HyoshiActivity`, `hyoshi-app.gradle` (a game's Android build) |
| `cmake/` | CPM and pinned dependencies, warnings, shaders, `hyoshi_add_app` (HyoshiApp.cmake), `hyoshi_add_code_checks` (Tooling.cmake) |
| `tests/unit`, `tests/replays` | doctest unit tests; replay fixtures |

### How a game plugs in

A game implements `hyoshi::app::Game` and passes it to `hyoshi::app::Application` with an `AppConfig` (name, window icon, whether to show the splash). `Application::Run` brings up the window, device, and audio, then calls `Game::Initialize` with the `AppServices` (platform, device, audio, jobs), shows the splash, and asks `Game::CreateFirstScene`. Each frame it calls `Game::Update`, then the scene's `RunFrame`; a scene moves on with `SwitchTo(nextScene)`, and the switch happens after the frame, with a fade. On backgrounding and at exit it calls `Game::Save`. The engine's assets install under `engine/` in the app's assets (`hyoshi_add_app`), a game's under their own folder names.

### How time flows

1. The audio callback publishes an `AudioClockSnapshot` (music frame, host time, seek generation) through a seqlock.
2. `SongPlayer::Update` feeds it to `SongClock`, which keeps a filtered mapping `songTime = hostUs + mappingOffset` anchored to the music cursor.
3. Input events carry OS timestamps in host time. A game converts them with `HostTimeToSongTime` and passes them to the judge (`ManiaJudge::ProcessAction`).
4. Each frame, the judge advances to 20 ms behind the current song time, to judge misses and hold ends.
5. Sounds that belong to notes are scheduled on music frames with `PlayAtMusicFrame`, so they follow pauses and seeks.

### How a frame flows

`Application::Run`: pump events, take the input queue, F1 toggles the overlay, `SpriteBatch::Begin`, `Game::Update`, then `RunScene`: `Ui::BeginFrame` reads the input, and the scene's `RunFrame` updates and draws through the UI in one call. A scene's `SwitchTo` takes effect after the frame; `EndScene` first logs the old one's summary and keeps its failed check and backward steps for the exit checks. `RenderFrame` then adds the stress sprites and the fade, flushes new glyphs (`TextRenderer::Flush`), and ends the sprite batch.

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
- **Trackpads.** SDL reports Mac trackpad fingers as touches from an indirect device. `Platform` drops those, so only direct devices (touchscreens) produce touch input.
- **stb_truetype's SDF and cubic curves.** `stbtt_GetGlyphSDF` ignores cubic segments, so CFF fonts (Noto Sans JP's `.otf`) lost their round letters. `TextRenderer` computes the field itself from the flattened outline; don't switch back.
- **Sprite order within a layer.** `SpriteBatch` sorts a layer's sprites by texture, so a rounded panel (circle texture), a plain rectangle (white texture), and text (atlas pages) on one layer draw in texture order, not call order. Stack things on different layers.
- **MSVC vs clang.** Each accepts things the other rejects: libc++ includes some standard headers transitively that MSVC's library doesn't (include what you use), and clang rejects a forward declaration that collides with a using-declaration where MSVC compiled it (clang-tidy caught it). Build or tidy with both.
- **Windows: CRLF checkout.** With `core.autocrlf=true`, the working tree has CRLF line endings and commits are normalized to LF. The build, tests, and format-check pass either way.
- **Windows: "'vswhere.exe' is not recognized".** The developer environment script prints this on every entry. It's harmless.
- **Windows: key timestamps are coarse.** SDL 3.4 stamps Windows key and mouse messages from `GetMessageTime`, which moves in 15 to 16 ms steps, as coarse as the ±16 ms Perfect window. The likely fix is raw input (`WM_INPUT`) on a dedicated thread, stamped with `QueryPerformanceCounter` (DESIGN.md section 12.3).
- **Android: bionic's `PAGE_SIZE` macro.** Bionic defines `PAGE_SIZE` in `<bits/page_size.h>`, which the standard headers pull in, so a constant by that name fails to compile (it was `TextRenderer`'s; now `ATLAS_PAGE_SIZE`). Avoid names that are common C macros.
- **Android: no floating-point `std::from_chars`.** The NDK's libc++ (LLVM 18) only has the integer overloads. `OsuManiaImporter`'s `ParseNumber` falls back to `strtod` for libc++ before 20; nothing may call `setlocale`, or that fallback reads commas.
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

1. Build and check on macOS (not done since mid M4).
2. A real Android phone: M1–M4 on the device (touch, cutouts, 20 background/foreground cycles, rotation), AAudio timestamps for the true output latency, and bundling the validation layer in debug builds.
3. The clock after a device restart (above). It changes `SongClock`, so it affects every platform.
4. GPU note positioning (DESIGN.md section 10.5). Notes are placed on the CPU now.
5. Offsets per output route (DESIGN.md section 11.5), and a calibration scene (M5).
6. CI (GitHub Actions) running the unit tests and replay fixtures on macOS and Windows, Tracy, ADR 0001.
7. Windows, when it matters for play: precise key timestamps.
