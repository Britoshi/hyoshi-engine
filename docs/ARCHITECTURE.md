# Architecture

How Hyoshi Engine is put together. For building and using the engine, see the [README](../README.md); for the full design and roadmap, see [DESIGN.md](DESIGN.md).

## Modules

Each module is a CMake target, `hyoshi::<module>`. `hyoshi::engine` links all of them.

| Path | Contents |
|---|---|
| `engine/core` | `Result<T>`, logging, asserts, generational handles, `Time.h` (integer microsecond and frame math), `Env.h` (environment variable helpers), `SpscQueue`, `SeqLock`, `JobSystem` |
| `engine/platform` | SDL3: the window, events into `InputQueue` (keys with repeats marked, wheel, touch, pointer, Mac trackpad fingers), pointer lock, app lifecycle, host clock, storage paths (base, user data, assets, shared), safe area, orientation, window icon, files dropped on the window (`TakeDroppedFiles`) or, on the web, chosen in the browser's file picker (`BrowseForFiles`), hiding the system cursor for a game that draws its own (`SetCursorHidden`), `LoadFile`, `SaveFile` (atomic), `MakeDirectory`. On the web, the user data folder is kept in the browser's IndexedDB (`platforms/web/pre.js`) and copied back after files change |
| `engine/input` | `InputEvent`, `InputQueue` (sorted by host time), `Trackpad` (maps an area of a trackpad onto the window, as tablet drivers do; `TrackpadFingers` tracks which finger aims) |
| `engine/rhi` | `IRenderDevice` and its Vulkan backend (volk, VMA; MoltenVK on macOS): swapchain with pre-rotation, pipeline cache, validation in debug builds. Web builds (Emscripten) use the WebGPU backend instead, with shaders compiled to WGSL |
| `engine/renderer` | `Camera2D` (the short side of the screen is 1080 units), `SpriteBatch` (sprites can be distance fields; `KeepOrder` draws a layer in submission order), `TextRenderer` (distance-field glyphs generated at runtime from TTF/OTF, in atlas pages), `Image` (PNG/JPEG, distance fields from alpha, textures) |
| `engine/debug` | The Dear ImGui overlay. Window positions are saved in `imgui.ini` in the user data folder, or `editor-imgui.ini` in the editor, with docking on. ImGui sets the system cursor only while its windows show. |
| `engine/audio` | `IAudioBackend`, `Mixer` (runs in the audio callback), `MusicStream` (music decoded ahead of playback on its own thread), the miniaudio backend and decoders, `AsioOutput` (an ASIO driver in the device's place, Windows; `ListAsioDrivers`), `Synth` (clicks and a metronome track) |
| `engine/rhythm` | `SongClock`, the chart format (`ChartFile`), `ScrollMap`, `Judgment`, `ScoreSystem`, `OffsetCalibration` |
| `engine/songs` | `Charts` (`ChartSource`, `GetChartKey`, a built-in 4-key chart on the metronome track, loading `.osu` and `.rchart.json` as Mania and `.osu` as Circle), `SongLibrary` (scans song folders on a worker thread with a mode's chart reader, from `LibraryOptions`; `Import` adds osu! map sets), `Osz` (`UnpackOsz`: a `.osz` into a new map folder, with miniz), `SongPlayer` (streams a song's music and keeps the song clock in step with it) |
| `engine/ui` | `Ui` (immediate-mode widgets, layout rectangles, colors, layers), `Logo` (images drawn as distance fields, scaled to fit) |
| `engine/app` | `Application` (the main loop, scene switching with a fade, lifecycle, debug hooks, exit checks), `Game` (the interface a game implements; `DrawOverlay` draws over every scene, on `layers::CURSOR`), `EditorLayer` (the editor's hook into the main loop), `Scene`, `ClockDemo`, the splash screen |
| `engine/editor` | Desktop only. `Editor` (the interface a game's editor implements), `Panel`, `EditorServices`, `Run`; the menu bar and dock space |
| `modes/mania` | `ManiaChart`, `ManiaJudge` (with autoplay), `ManiaPlayfield` |
| `modes/circle` | `CircleChart` (slider paths, stacking, combo numbers), `CircleJudge` (with autoplay), `CircleScore`, `CirclePlayfield` (`CircleLayout` fits osu!'s 512×384 playfield to the screen) |
| `tools/osu-import` | `ImportOsuMania`, `ImportOsuStandard` (shared parsing in `OsuText.h`), and the `hyoshi-osu-import` command-line tool |
| `tools/asset-cooker` | `hyoshi-asset-cooker icons`: a game's icons from its logo |
| `samples/metronome` | The smallest game: the splash, then the clock demo |
| `platforms/windows` | The UTF-8 code page manifest embedded in every executable |
| `platforms/android` | SDL's Java code (`org/libsdl/app`, from SDL 3.4.16), `HyoshiActivity`, and `hyoshi-app.gradle` for a game's Android build |
| `cmake/` | CPM and pinned dependencies (ImGui is the docking branch), compiler warnings, shader compilation, `hyoshi_add_app` and `hyoshi_add_editor` (`HyoshiApp.cmake`), `hyoshi_add_code_checks` (`Tooling.cmake`) |
| `tests/unit`, `tests/replays` | doctest unit tests and replay fixtures |

## How a game plugs in

A game implements `hyoshi::app::Game` and passes it to `hyoshi::app::Application` with an `AppConfig` (name, window icon, whether to show the splash).

`Application::Run` creates the window, the GPU device, and the audio device, then calls `Game::Initialize` with the `AppServices` (platform, device, audio, jobs). It shows the splash and then asks `Game::CreateFirstScene` for the first scene. Each frame it calls `Game::Update` and then the scene's `RunFrame`. A scene moves to the next with `SwitchTo(nextScene)`; the switch happens after the current frame, with a fade. When the app goes to the background and at exit, it calls `Game::Save`.

The engine's assets are installed under `engine/` in the app's assets (by `hyoshi_add_app`), and a game's assets under their own folder names.

### The editor

A game's editor is a second executable built from the same game code with `hyoshi_add_editor`. Its `main` calls `editor::Run(config, game, editor)`, which gives the `Application` an `EditorLayer` and skips the splash. With an editor layer, the `Application`:

- keeps ImGui on, with docking and its own layout file (`editor-imgui.ini`);
- lets ImGui windows take the keyboard and pointer while they need them;
- sets `AppServices::IsEditor`;
- calls the layer's `Initialize` after `Game::Initialize` (so the game's services exist), its `Build` every frame before the debug overlay's windows, and its `Shutdown` before `Game::Shutdown`.

The layer draws the menu bar (File, View), a dock space over the window whose empty center passes input through to the game, and the game's panels. `AppConfig::WindowTitle` lets the editor's title differ from the game's while it shares the game's user data folder (`AppConfig::Name`). See [decision 0002](decisions/0002-editor-framework.md) for why it works this way.

## How music is loaded

`SongPlayer::LoadFile` reads the file and opens a `MusicStream` on a worker, which takes a few milliseconds: the stream reads the headers and knows the length. The stream's thread then decodes about 1.4 s ahead in fixed blocks, which pass to the audio thread and back through lock-free queues. A seek starts a new epoch: the mixer drops blocks decoded for the old position, and until the first block for the new one arrives, it holds the music and reports it as not playing, which the song clock treats as a pause. Seeks land on the frame that decoding from the start reaches; MP3s get there by decoding, since miniaudio's MP3 seeks miss by up to 26 ms (DESIGN.md section 11.2). Sounds, and music generated in memory (`SongPlayer::Load`), are decoded whole instead.

## How time flows

1. The audio callback publishes an `AudioClockSnapshot` (music frame, host time, seek generation) through a seqlock.
2. `SongPlayer::Update` passes it to `SongClock`, which keeps a filtered mapping `songTime = hostUs + mappingOffset`, anchored to the music's playback position.
3. Input events carry the OS's timestamps in host time. A game converts them with `HostTimeToSongTime` and passes them to the judge (for example `ManiaJudge::ProcessAction`).
4. Each frame, the judge advances to 20 ms behind the current song time, to find misses and the ends of held notes.
5. Sounds that belong to notes are scheduled on music frames with `PlayAtMusicFrame`, so they follow pauses and seeks.

## How a frame runs

`Application::Run` pumps events, takes the input queue, toggles the overlay on F1 and fullscreen on F11 or Alt+Enter (that Enter doesn't reach the scene), begins the sprite batch, and calls `Game::Update`. Then `RunScene` begins the UI frame (which reads the input), and the scene's `RunFrame` updates and draws in one call.

A scene's `SwitchTo` takes effect after its `RunFrame`. `EndScene` logs the old scene's summary and keeps its failed checks and backward time steps for the exit checks, and the new scene runs from the next frame. The old scene is destroyed only after the frame has been recorded, because its sprites, and therefore its textures, are still in the frame's sprite batch.

`RenderFrame` then adds the stress-test sprites and the fade, uploads new glyphs (`TextRenderer::Flush`), and ends the sprite batch.

## Design rules

Changes must keep these true.

- **Gameplay time is integer microseconds** (`SongTimeUs`, `HostTimeNs`). Never compare times as floating point.
- **Song time never goes backwards,** except across a seek. `SongPlayer` counts backward steps, and a debug run exits with a failure code if there were any.
- **Judgment is deterministic.** It depends only on the chart and the timestamped actions. Live play and a replay of the recorded actions must produce the same events; a unit test checks this.
- **The offset sign.** `songTime = audibleMusicTime − outputLatency − userAudioOffset − chartOffset`. A positive user offset makes notes later, which corrects for hitting late. Calibration adds the median error to the offset.
- **`BuildManiaChart` clears `Info.Notes`.** `ManiaChart::Notes` is the only copy of the notes; `ManiaNote` carries the sample fields.
- **The engine never writes to song folders,** with two exceptions: the `dev-songs/` folder that debug APKs unpack (`UnpackBundledSongs`), which is in the app's private data, and map sets the player adds (`SongLibrary::Import`), which only ever create a new map folder and never write into one that's already there.
- **Release APKs never include a game's development maps.** The `bundleDevSongs` task in `hyoshi-app.gradle` runs for debug builds only.
- **The engine never depends on a game.** Games use the engine's public headers and CMake functions; nothing under `engine/`, `modes/`, or `tools/` refers to a specific game.
