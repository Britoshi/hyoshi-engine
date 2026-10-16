# Hyoshi Engine Design Document

| | |
|---|---|
| **Name** | Hyoshi (namespace `hyoshi`) |
| **Owner** | Brian Kim (Britoshi) |
| **Status** | Draft v0.1 |
| **Last updated** | September 24, 2026 |
| **Language** | C++20 |
| **Primary targets** | Android (Vulkan), macOS (Vulkan via MoltenVK, development host) |
| **Later targets** | iOS (native Metal backend), Windows, Linux |

---

## Table of Contents

1. [Vision](#1-vision)
2. [Goals and Non-Goals](#2-goals-and-non-goals)
3. [Guiding Principles](#3-guiding-principles)
4. [Technology Stack](#4-technology-stack)
5. [Architecture Overview](#5-architecture-overview)
6. [Repository Layout](#6-repository-layout)
7. [Platform Layer](#7-platform-layer)
8. [Threading Model](#8-threading-model)
9. [Time and Clocks](#9-time-and-clocks)
10. [Rendering](#10-rendering)
11. [Audio](#11-audio)
12. [Input](#12-input)
13. [Chart Format](#13-chart-format)
14. [Rhythm Core](#14-rhythm-core)
15. [Timeline Animation System](#15-timeline-animation-system)
16. [Game Modes](#16-game-modes)
17. [Game Object Model, Scenes, and UI](#17-game-object-model-scenes-and-ui)
18. [Assets and Content Pipeline](#18-assets-and-content-pipeline)
19. [Tools and Debugging](#19-tools-and-debugging)
20. [Mobile Requirements Checklist](#20-mobile-requirements-checklist)
21. [Coding Standards](#21-coding-standards)
22. [Build, CI, and Deployment](#22-build-ci-and-deployment)
23. [Testing Strategy](#23-testing-strategy)
24. [Performance Budgets](#24-performance-budgets)
25. [Roadmap and Milestones](#25-roadmap-and-milestones)
26. [Getting Started: Week One](#26-getting-started-week-one)
27. [Risks and Mitigations](#27-risks-and-mitigations)
28. [Open Decisions](#28-open-decisions)
29. [Decision Log](#29-decision-log)
30. [Glossary](#30-glossary)

---

## 1. Vision

A **mobile-forward C++ engine purpose-built for rhythm games**, architected to a standard a professional studio could adopt. The engine treats audio timing as its foundation rather than an afterthought, and it is general enough to host every major family of rhythm game.

The engine's scope is validated against five **reference game styles**:

| Style | Reference | What it proves |
|---|---|---|
| Mania | osu!mania, generic VSRG | The core loop: clock, chart, scrolling notes, lane judgment |
| Drum | Taiko | Modes are truly pluggable; hit types beyond "press lane N" |
| Circle | osu!standard | No lanes; positional judgment; curved slider paths; cursor input |
| Highway | Sound Voltex | Perspective rendering; analog input; camera effects synced to chart |
| Line | Phigros | Animated judgment lines; multi-touch gestures; heavy chart-driven animation |

> **Important:** reference styles are internal validation targets. Any game shipped on this engine must have its own identity: original names, art, UI, and music. No reference game's trademarks, assets, or skins ship with the engine.

---

## 2. Goals and Non-Goals

### Goals

- **Accurate timing above all.** Judgment is driven by the audio clock and timestamped input, never by frame timing.
- **Mobile first.** Android is the primary device target from day one; every system is designed for touch, tile-based GPUs, thermals, battery, and unpredictable app lifecycles.
- **Pluggable game modes.** The engine core knows about timed events, not lanes. Each game style is a mode module.
- **Swappable backends.** Rendering sits behind an RHI (Vulkan now, Metal later). Audio sits behind a backend interface (miniaudio now, FMOD or Wwise optional later).
- **Deterministic judgment.** Given a chart and a list of timestamped inputs, the result is always identical. This enables replays, automated tests, and anti-cheat.
- **Studio-grade structure.** Clear module boundaries, permissive dependencies, desktop tooling, documented interfaces.
- **A real game ships on it.** The engine grows by building actual playable modes, not in isolation.

### Non-Goals (v1)

- General-purpose 3D engine features (skeletal animation, physics, large worlds, lighting pipelines)
- ECS architecture (see Decision Log)
- Online multiplayer, accounts, leaderboards (may come later as services, not engine core)
- Console support
- Visual scripting
- Hot-swapping audio or graphics backends at runtime (backends are chosen at build time)
- A full in-engine editor in v1 (tools start as ImGui panels and grow)

---

## 3. Guiding Principles

1. **The audio clock is the source of truth.** Song time is derived from how many samples the audio device has consumed, corrected for latency. Everything else (visuals, judgment, animation) follows it.
2. **Timestamp at the source.** Input is timestamped by the OS when it happens, not when the engine gets around to processing it.
3. **Data-oriented hot paths, object-oriented authoring.** Notes, particles, and render data live in tight arrays processed in bulk. Gameplay objects, scenes, and UI use a composition-based object model.
4. **Abstract at the boundaries, not everywhere.** RHI, audio backend, platform layer, and game modes are interfaces. Internal code stays concrete and simple.
5. **Measure before optimizing.** Tracy is integrated from the start. No threading, custom allocators, or clever structures without a profile that justifies them.
6. **The weakest device is the baseline.** Performance is judged on the lowest-end test phone, after 10 minutes of sustained play.
7. **Tools live on desktop, the game runs on device.** The development loop is: edit on Mac, run on Mac for speed, verify on Android for truth.

---

## 4. Technology Stack

### Core stack

| Area | Choice | Notes |
|---|---|---|
| Language | C++20 | Custom `Result<T>` type instead of `std::expected` (C++23) |
| Build | CMake 3.28+ with presets | Dependencies via CPM.cmake |
| Platform, window, input, lifecycle | SDL3 | Covers Android, macOS, iOS, Windows, Linux |
| Graphics API | Vulkan 1.1 baseline + extensions | Android Baseline Profile compatible; MoltenVK on macOS |
| Vulkan loader | volk | Meta-loader, avoids linking against the loader directly |
| GPU memory | Vulkan Memory Allocator (VMA) | |
| Vulkan bootstrap | vk-bootstrap (optional) | Speeds up instance, device, and swapchain setup |
| Shaders | Slang, compiled offline to SPIR-V | Slang can also emit Metal later |
| Audio | miniaudio | AAudio on Android, CoreAudio on Apple |
| Audio decoding | stb_vorbis (Ogg Vorbis), dr_wav, dr_flac | Opus via libopus later if needed |
| Math | GLM | |
| Debug UI and early tools | Dear ImGui | SDL3 + Vulkan backends |
| Profiling | Tracy | Works on Android over the network |
| Logging | spdlog | Routes to logcat on Android |
| JSON | yyjson | Fast, C, MIT |
| Hashing | xxHash | Asset IDs and chart hashes |
| Images | stb_image | Source images in tools; cooked textures at runtime |
| Texture compression | astcenc (offline) | ASTC for mobile |
| Fonts | msdf-atlas-gen (offline) | Multi-channel SDF text |
| Unit tests | doctest | |

### License check

Every runtime dependency must be permissively licensed (MIT, zlib, BSD, Apache 2.0, public domain). No GPL or LGPL in the runtime. Verify each license at the version you pin before shipping; licenses occasionally change between releases.

| Dependency | Expected license (verify) |
|---|---|
| SDL3 | zlib |
| miniaudio | Public domain or MIT-0 |
| volk, VMA, GLM, Dear ImGui, spdlog, yyjson, doctest | MIT |
| vk-bootstrap | MIT |
| Tracy | BSD 3-Clause |
| xxHash | BSD 2-Clause |
| stb libraries, dr_libs | Public domain or MIT |
| cgltf (glTF models, as built) | MIT |
| astcenc | Apache 2.0 (tool only, not shipped) |
| Slang | Apache 2.0 with LLVM exception (compiler used offline) |
| Noto Sans JP (UI font, as built) | SIL Open Font License 1.1: may be bundled with software, with the license alongside |

---

## 5. Architecture Overview

### Layer diagram

Each layer may only depend on layers below it.

```
+-------------------------------------------------------------------+
|  GAME / APPLICATION                                               |
|  Scenes (Title, SongSelect, Gameplay, Results, Calibration), UI   |
+-------------------------------------------------------------------+
|  GAME MODES (plugins)                                             |
|  Mania | Drum | Circle | Highway | Line                           |
+-------------------------------------------------------------------+
|  RHYTHM CORE                                                      |
|  SongClock | Chart | NoteCursor | Judgment | Scoring | Replay     |
|  Timeline animation                                               |
+-------------------------------------------------------------------+
|  ENGINE SUBSYSTEMS                                                |
|  Renderer (SpriteBatch, Text, Cameras) | Audio | Input | Assets   |
|  Object model (Entities, Components, Scenes) | Debug UI          |
+-------------------------------------------------------------------+
|  BACKENDS (chosen at build time)                                  |
|  RHI: Vulkan (Metal later) | Audio: miniaudio (FMOD later)        |
+-------------------------------------------------------------------+
|  CORE                                                             |
|  Memory | Containers | Handles | Logging | Asserts | Math | Jobs  |
+-------------------------------------------------------------------+
|  PLATFORM                                                         |
|  SDL3: window, events, lifecycle, filesystem, threads, time       |
+-------------------------------------------------------------------+
```

### Module responsibilities

| Module | Owns | Must never |
|---|---|---|
| Platform | OS interaction, lifecycle events, raw input, file access | Contain gameplay or rendering logic |
| Core | Foundational utilities used everywhere | Depend on any other engine module |
| RHI | GPU resource creation, command recording, presentation | Know what a sprite, note, or scene is |
| Renderer | Batching, text, cameras, render ordering | Call Vulkan directly (only through RHI) |
| Audio | Device, mixing, playback, the DSP clock | Allocate or lock inside the audio callback |
| Input | Timestamped event queue, device abstraction | Interpret events as gameplay actions |
| Rhythm Core | Song time, charts, judgment framework, replays | Know about specific modes |
| Game Modes | Mode-specific chart data, input mapping, judgment rules, playfield rendering | Touch backends directly |
| Game | Scenes, menus, flow | Reach below the Engine Subsystems layer |

---

## 6. Repository Layout

```
hyoshi-engine/
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── .clang-tidy
├── cmake/
│   ├── CPM.cmake
│   ├── Dependencies.cmake
│   └── Shaders.cmake              # Slang -> SPIR-V build step
├── engine/
│   ├── core/                      # memory, containers, handles, log, assert, math, jobs
│   ├── platform/                  # SDL3 wrapper, lifecycle, filesystem, time
│   ├── rhi/
│   │   ├── include/rhi/           # backend-agnostic interface
│   │   ├── vulkan/                # Vulkan backend
│   │   └── metal/                 # (later) Metal backend
│   ├── renderer/                  # sprite batch, text, cameras, render layers
│   ├── audio/
│   │   ├── include/audio/         # IAudioBackend and public types
│   │   └── miniaudio/             # miniaudio backend
│   ├── input/
│   ├── assets/
│   ├── rhythm/                    # clock, chart, cursor, judgment, scoring, replay, timeline
│   ├── scene/                     # entities, components, scene management
│   └── debug/                     # ImGui integration, debug overlays
├── modes/
│   ├── mania/
│   ├── drum/
│   ├── circle/
│   ├── highway/
│   └── line/
├── game/                          # the actual game built on the engine
│   ├── scenes/
│   └── ui/
├── shaders/                       # .slang sources
├── content/                       # source assets (dev only, not shipped raw)
│   ├── charts/
│   ├── audio/
│   ├── textures/
│   └── fonts/
├── tools/
│   ├── asset-cooker/              # textures to ASTC, fonts to MSDF, charts to binary
│   └── chart-editor/              # (later) desktop chart editor
├── platforms/
│   ├── android/                   # Gradle project, manifest, SDL3 Java glue
│   └── apple/                     # (later) Xcode project generation, plist
├── tests/
│   ├── unit/
│   └── replays/                   # replay regression fixtures
└── docs/
    ├── DESIGN.md                  # this document
    └── decisions/                 # one file per architecture decision record (ADR)
```

As built (2026-09-28, the engine split): games live in their own repositories and add this one as a git submodule. The engine keeps `engine/` (with three modules the plan didn't list: `ui`, the immediate-mode UI kit; `songs`, chart loading, the song library, and `SongPlayer`; and `app`, the application framework with the `Game` interface), `modes/`, `tools/`, `shaders/`, `cmake/`, `tests/`, `docs/`, a `samples/` folder, `content/textures/` with the engine's logos, and `platforms/` with only per-platform glue (the Windows manifest, SDL's Android Java sources, the engine's activity, and `hyoshi-app.gradle`). `game/`, the game's content, and its Android and Windows projects moved to the game's repository (as `src/`, `content/`, `android/`, and `windows/`). The README's "Making a game" shows the layout a game uses. `assets/` and `scene/` don't exist; scenes are `app::Scene`.

---

## 7. Platform Layer

The platform layer wraps SDL3 so no other module includes SDL headers directly (except the RHI surface creation code and ImGui integration).

### Responsibilities

- Window and surface creation (Vulkan surface via `SDL_Vulkan_CreateSurface`)
- Event pump: keyboard, touch, gamepad, window, and lifecycle events
- Filesystem: app bundle or APK asset reads (SDL `SDL_IOStream` handles Android assets), writable user data directory
- High resolution time: `SDL_GetTicksNS()` is the engine's **host clock**
- Thread primitives

### Main loop

```cpp
void Application::Run()
{
    while (isRunning)
    {
        platform.PumpEvents(inputQueue, lifecycleHandler);

        if (isSuspended)
        {
            platform.WaitForEvents();
            continue;
        }

        songClock.Update(platform.GetHostTimeNs());
        sceneManager.Update(songClock);
        renderer.BeginFrame();
        sceneManager.Render(renderer);
        debugUi.Render(renderer);
        renderer.EndFrame();
    }
}
```

### App lifecycle (critical on mobile)

| SDL3 event | Engine response |
|---|---|
| `SDL_EVENT_WILL_ENTER_BACKGROUND` | Pause gameplay, pause audio, stop rendering, save pipeline cache and user settings |
| `SDL_EVENT_DID_ENTER_BACKGROUND` | Release swapchain and surface (Android destroys the native window) |
| `SDL_EVENT_WILL_ENTER_FOREGROUND` | Prepare to recreate surface |
| `SDL_EVENT_DID_ENTER_FOREGROUND` | Recreate surface and swapchain, resume audio device, show pause menu (never auto-resume a song) |
| `SDL_EVENT_LOW_MEMORY` | Drop caches (song previews, unused textures) |
| `SDL_EVENT_TERMINATING` | Flush saves, shut down cleanly |

These events are handled in an `SDL_AddEventWatch` callback, not in the event pump: on mobile, the OS may suspend the app before the main loop pumps them. The callback can run off the main thread (on Android, the Java UI thread), so the response must be short and thread-safe.

Rule: **a song never resumes automatically after an interruption.** The player always returns to a pause screen with a countdown, and the clock resyncs before gameplay continues.

As built: the event watch only flags the suspension (`Platform::IsSuspended`, and `ConsumeSuspension`, true once after any trip to the background), and the main loop responds: it pauses the scene and its song, suspends the audio device, saves, releases the surface, and waits for events until the app is back, then resumes the device and makes a new surface. On Android, SDL by default blocks inside its event pump while the app is paused, so a whole trip to the background and back passed within one `PumpEvents` and the loop never saw it: the song played on in the background, and the old surface, its window gone, stayed black on return. The platform sets `SDL_HINT_ANDROID_BLOCK_ON_PAUSE` to 0, so the pump returns and the loop sees the suspension, and `ConsumeSuspension` catches a trip shorter than a frame. If presenting still reports `VK_ERROR_SURFACE_LOST_KHR`, the renderer releases the surface and the loop makes a new one. Not yet: a pause that lands during startup (the phone locking as the app opens) loses the surface before the first swapchain, and startup fails.

---

## 8. Threading Model

Start with the minimum number of threads and add more only when profiling demands it.

| Thread | Owns | Rules |
|---|---|---|
| **Main** | SDL event pump, game logic, rhythm core, render command recording and submission | The only thread that touches scenes and gameplay state |
| **Audio** | miniaudio callback: mixing music and hit sounds, advancing the DSP clock | Real-time safe: no allocations, no locks, no file IO, no logging. Communicates only through lock-free queues and atomics |
| **Workers (job system)** | Asset loading, audio decoding, chart parsing, texture uploads staging | Never touch gameplay state directly; results handed back to main via queues |

### Explicitly not threaded (v1)

- **Note processing and judgment.** Per-frame work is tiny (only notes near the judgment window), and it must be sequential and deterministic. Note visuals are computed on the GPU instead.
- **Render thread.** Main thread recording is enough for a rhythm game's draw load. Revisit only if profiling shows the CPU side of rendering is the bottleneck.

### Communication primitives

- `SpscQueue<T>`: single producer, single consumer lock-free ring buffer (audio thread to main, input to main)
- `std::atomic` for the audio clock snapshot (see Section 9)
- Job system: fixed pool of worker threads (`hardware_concurrency - 2`, minimum 1), simple job queue with completion counters

---

## 9. Time and Clocks

This is the most important section of the engine.

### Time units and types

```cpp
using HostTimeNs = int64_t;   // SDL_GetTicksNS() domain, monotonic
using SongTimeUs = int64_t;   // song time in microseconds, 0 = audio file start
using DspFrame   = int64_t;   // audio sample frames consumed by the device
```

Rules:

- **Charts, judgment, and replays use `SongTimeUs` (integers only).** No floating point in judgment math, which keeps results deterministic across devices.
- Rendering may convert to `float` or `double` seconds at the last moment.
- Frame delta time is **never** used for anything timing-related in gameplay. It is only allowed for purely cosmetic effects (particle drift, UI easing).

### The three clocks

| Clock | Source | Used for |
|---|---|---|
| Host clock | `SDL_GetTicksNS()` | Common reference that input timestamps and audio snapshots share |
| DSP clock | Frames consumed by the audio callback | Ground truth of playback progress |
| Song clock | Derived: DSP clock mapped through host time, corrected by latency and offsets | Everything gameplay related |

### Audio clock snapshot

At the start of every audio callback, the audio thread publishes a snapshot:

```cpp
struct AudioClockSnapshot
{
    DspFrame FramesConsumed;     // total frames handed to the device before this callback
    HostTimeNs HostTime;         // SDL_GetTicksNS() at callback start
    int32_t SampleRate;
};
```

Published via a seqlock or double buffer with an atomic index so the main thread always reads a consistent snapshot without locking.

### Deriving song time on the main thread

```
estimatedFrames = snapshot.FramesConsumed
                + (nowHost - snapshot.HostTime) * sampleRate / 1e9

rawSongTimeUs   = (estimatedFrames - songStartFrame) * 1e6 / sampleRate
                - outputLatencyUs          // device + route latency estimate
                - userAudioOffsetUs        // calibration
                - chartOffsetUs            // per-chart offset
```

### Smoothing

Audio callbacks arrive in bursts (one per buffer period), so the raw estimate jitters. The `SongClock` smooths it:

- Advance a smoothed clock by host time every frame.
- Compare against the raw estimate; correct drift gradually (for example, apply 5 to 10 percent of the error per frame).
- If the error exceeds a hard threshold (for example 30 ms, which happens after a seek or a hitch), snap immediately.
- **The smoothed clock must never move backwards** during normal playback.

```cpp
class SongClock
{
public:
    void Start(DspFrame songStartFrame);
    void Update(HostTimeNs now);
    void Pause();
    void Resume();
    void Seek(SongTimeUs target);

    SongTimeUs GetSongTime() const;                          // smoothed, for visuals
    SongTimeUs HostTimeToSongTime(HostTimeNs hostTime) const; // for judging input

    int64_t OutputLatencyUs = 0;
    int64_t UserAudioOffsetUs = 0;
    int64_t UserVisualOffsetUs = 0;
    int64_t ChartOffsetUs = 0;

private:
    SongTimeUs smoothedTime = 0;
    HostTimeNs lastUpdateHost = 0;
    bool isPaused = true;
};
```

### Offsets

| Offset | Compensates for | Applied to | Set by |
|---|---|---|---|
| Output latency | Audio buffer and device latency | Everything | Engine estimate (Section 11) |
| User audio offset | Remaining audio latency, especially Bluetooth | Judgment and visuals | Calibration tap test |
| User visual offset | Display latency | Visuals only | Calibration visual test |
| Chart offset | Chart authored against slightly shifted audio | Everything | Chart file |

### Converting input to song time

Input events carry host timestamps (Section 12). Judgment converts them with `HostTimeToSongTime`, which uses the **raw mapping** (not the smoothed display clock), so judgment accuracy never depends on smoothing.

### As built (M3)

The implementation (`engine/rhythm/SongClock`, `engine/audio/Mixer`) refines the plan above:

- **Anchored to the music cursor.** The snapshot also carries `MusicFrame` (the music position of the first frame the callback renders), `IsMusicPlaying`, and a `SeekGeneration` that every seek and music change increments. Song time is `MusicFrame` extrapolated by host time since the callback, minus latency and offsets, instead of `FramesConsumed - songStartFrame`, so pause, seek, and device restarts can't desynchronize it. Transport lives in `IAudioBackend` (`PlayMusic`, `PauseMusic`, `SeekMusic`), and `SongClock` follows the snapshots instead of having its own `Start`/`Pause`/`Seek`.
- **One filtered mapping for visuals and judgment** (decision 16). Snapshots are timestamped when the callback starts, so the raw estimate jitters by the OS's callback scheduling. The clock keeps `songTime = hostTime + offset`, corrects 10% of the error per update (at most 1 ms), and re-anchors past 30 ms. `HostTimeToSongTime` uses that mapping, which converges on the average of the raw estimates and is therefore more accurate than any single one.
- **Never backwards except across a seek.** When a re-anchor lands behind the time already shown, song time holds until the music catches up rather than rewinding.
- **Stalls.** A snapshot older than 60 ms (or four callbacks) while playing means the device stopped; the clock holds instead of running ahead of the music.
- **Seeks.** `ExpectSeek(generation, target)` shows the target from the same frame, before the audio thread has applied it.
- **Music rate.** At a music rate other than 1 (`IAudioBackend::SetMusicRate`, an editor's slowed or sped-up preview), the snapshot reports the rate, and song time runs at it: the mapping becomes song time = the anchor's song time + (host time − the anchor's host time) × rate. The latencies (output and the player's audio offset) are real time, so they cover rate times as much of the music; the chart's offset is music time and doesn't scale. A change of rate re-anchors, as a seek does, and `SongPlayer` doesn't count the step as going backwards. New music is a seek to its first frame: `SongPlayer` expects it (`SongClock::ExpectSeek`) when the music it loaded is handed to the backend, so until the audio thread has it, the clock holds at the start rather than follow the music before it, which may still be playing. `HostTimeToSongTime` and `SongTimeToHostTime` round to the microsecond, so they're exact inverses only at 1.

---

## 10. Rendering

### 10.1 RHI (Rendering Hardware Interface)

Backend-agnostic, shaped around concepts that both Vulkan and Metal share.

**Resources** (all referenced by typed generational handles, never raw pointers):

```cpp
struct BufferHandle   { uint32_t Index; uint32_t Generation; };
struct TextureHandle  { uint32_t Index; uint32_t Generation; };
struct PipelineHandle { uint32_t Index; uint32_t Generation; };
struct SamplerHandle  { uint32_t Index; uint32_t Generation; };
```

**Core interface sketch:**

```cpp
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;

    virtual Result<void> Initialize(const DeviceConfig& config) = 0;
    virtual void Shutdown() = 0;

    // Surface lifecycle (mobile)
    virtual Result<void> CreateSurface(const PlatformWindow& window) = 0;
    virtual void DestroySurface() = 0;
    virtual Result<void> RecreateSwapchain() = 0;

    // Resources
    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual void Destroy(BufferHandle handle) = 0;
    virtual void Destroy(TextureHandle handle) = 0;
    virtual void Destroy(PipelineHandle handle) = 0;
    virtual void Destroy(SamplerHandle handle) = 0;

    virtual void UploadBuffer(BufferHandle handle, const void* data, size_t size, size_t offset) = 0;

    // Frame
    virtual FrameStatus BeginFrame() = 0;             // may report SwapchainOutOfDate
    virtual ICommandList& GetCommandList() = 0;
    virtual void EndFrameAndPresent() = 0;

    // Capabilities
    virtual const DeviceCapabilities& GetCapabilities() const = 0;
    virtual SurfaceTransform GetSurfaceTransform() const = 0;
};

class ICommandList
{
public:
    virtual ~ICommandList() = default;

    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;  // load/store actions per attachment
    virtual void EndRenderPass() = 0;
    virtual void BindPipeline(PipelineHandle pipeline) = 0;
    virtual void BindVertexBuffer(BufferHandle buffer, size_t offset) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type) = 0;
    virtual void BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) = 0;
    virtual void PushConstants(const void* data, uint32_t size) = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const Rect& scissor) = 0;
    virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                      uint32_t firstIndex, int32_t vertexOffset,
                                      uint32_t firstInstance) = 0;
};
```

Design notes:

- **Render passes with explicit load and store actions** map directly onto Vulkan render passes and Metal render pass descriptors, and are exactly what tile-based mobile GPUs need.
- **Barriers are the backend's job.** Resources are created with declared usage (`VertexBuffer`, `Sampled`, `ColorAttachment`, `TransferDst`), and the Vulkan backend inserts the necessary barriers internally. Metal handles most hazards automatically. This keeps the RHI simple while keeping Metal viable.
- **Capabilities are queried, not assumed** (ASTC support, max texture size, dynamic rendering availability).

### 10.2 Vulkan backend

| Topic | Decision |
|---|---|
| API baseline | Vulkan 1.1 core. Extensions used when available, never required unless listed |
| Loader | volk; `libvulkan.so` on Android, MoltenVK through the Vulkan SDK loader on macOS |
| macOS | Enable `VK_KHR_portability_enumeration` (instance) and `VK_KHR_portability_subset` (device) |
| Render passes | Traditional `VkRenderPass` and `VkFramebuffer` objects, created and cached by the backend from `RenderPassDesc`. Dynamic rendering is an optional future optimization, since many Android devices lack it |
| Frames in flight | 2 |
| Present mode | FIFO (guaranteed everywhere, battery friendly). IMMEDIATE on desktop when the game asks for low latency (`AppConfig::LowLatency`), or MAILBOX where the surface doesn't offer it |
| Memory | VMA. Depth and MSAA attachments use `TRANSIENT_ATTACHMENT` usage with lazily allocated memory, so they never leave tile memory on mobile |
| Descriptors | Per-frame descriptor pools reset each frame. No bindless (poor mobile support). Texture atlases keep bind counts low |
| Pipeline cache | `VkPipelineCache` saved to disk on background/exit and loaded on startup to avoid shader compile hitches |
| Validation | Validation layers bundled in debug APKs and enabled on desktop debug builds |
| Shaders | Slang compiled to SPIR-V at build time, embedded or shipped as assets |

**Android pre-rotation (must have):**

Android devices often report a surface in the display's natural orientation (usually portrait). If the swapchain does not match, the system compositor rotates every frame, which costs performance and adds latency.

1. Query `VkSurfaceCapabilitiesKHR::currentTransform`.
2. Create the swapchain with `preTransform = currentTransform`.
3. For 90 or 270 degree transforms, swap the swapchain extent's width and height.
4. Apply the matching rotation to the projection matrix so the game renders "pre-rotated."
5. Treat `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DATE_KHR` as signals to recreate the swapchain.

**Swapchain recreation triggers:** resize, rotation, `VK_ERROR_OUT_OF_DATE_KHR`, returning from background.

### 10.3 Renderer (above the RHI)

| Component | Purpose |
|---|---|
| `SpriteBatch` | Instanced quads with per-instance position, size, rotation, UV rect, color. One draw per texture atlas per layer. As built: each instance carries its four corners (a rotated sprite's are found on the CPU), so `DrawQuad` also draws any convex quadrilateral, such as a trapezoid seen in perspective, and `DrawTriangle` a triangle (a quad with two corners together): solid, or showing the part of a texture laid out under it, artwork cut to a shape without stretching (the instance's UV rect then says where the texture lies, in virtual units, and the vertex shader maps each corner into it). `DrawTriangleGlow` lights a triangle at its edges, fading inward: the vertex shader gives each corner its distances to the three sides (its barycentric coordinates times the triangle's heights), so each pixel knows how far it is from the nearest side, in virtual units at any size, and the outer edge is smoothed over a pixel. `SetOpacity` fades everything drawn after it (each sprite on its own), so a whole playfield can fade in or out, and `SetClip` shows everything drawn after it only inside a rectangle (each sprite carries it; the shader leaves pixels outside transparent), so a playfield shown in part of the screen keeps to its part |
| `TextRenderer` | MSDF glyph atlas; crisp at any size, cheap to render |
| `Camera2D` | Orthographic; virtual resolution with aspect-aware scaling |
| `Camera3D` | Perspective, for `Renderer3D` (as built: position, target, up, field of view, near and far; Vulkan's depth range and y flip, and the swapchain's pre-rotation) |
| `RenderLayer` | Sort key: `(layer, material/texture, depth)` to minimize state changes |
| `PlayfieldRenderer` | Interface each game mode implements |

As built (3D, decision 0003, [decisions/0003-real-3d.md](decisions/0003-real-3d.md), 2026-10-15; on the Mac only so far): the swapchain pass has a depth buffer on Vulkan (transient, lazily allocated, not kept), and `PipelineDesc` has depth, culling, and winding settings and additive blending; `IRenderDevice::HasDepth` is false on the web, which refuses them. `Renderer3D` (`AppServices::World`) draws meshes (`MeshVertex`, shapes made in code by `renderer/Mesh.h`'s `AddBox`, `AddCylinder`, `AddSphere`, `AddSurface`, `AddPlane`) with materials (color, texture, emission, rim, opaque, alpha, or additive, one- or two-sided) under one directional light, a sky-to-ground ambient, and fog, all in push constants (`shaders/Mesh.slang`). Scenes call `Draw` while they run; `RenderFrame` draws the list right after beginning the pass, solid first (nearest first), then see-through (farthest first), then additive, and the 2D draws over it. Up to four lightings in a frame, each material taking one (`Material::LightingSlot`); `SliceMesh` cuts meshes into slabs for effects; `renderer/Model.h` reads glTF models (static: meshes, materials, base color textures; cgltf). For Sonnet of Melody's title and menu, a 3D world. Still to come (decision 0003, milestone 5): multisampling, many lights, skinned and animated models, render targets, and depth on the web.

As built (text): `TextRenderer` (`engine/renderer`) draws single-line text from TrueType and OpenType fonts, with fallback fonts for missing glyphs. Instead of MSDF atlases cooked offline, glyphs are rasterized at runtime on first use: stb_truetype supplies the outline, whose curves (quadratic and cubic) are flattened into segments, and the renderer computes a single-channel signed distance field (48 pixels per em, 6 pixels of spread) into 2048 x 2048 R8 atlas pages. stb_truetype's own `stbtt_GetGlyphSDF` ignores cubic curves, which garbled the CFF-based Noto Sans JP. Glyphs are `SpriteBatch` sprites with a distance field flag, so text sorts with other sprites by layer; the sprite shader smooths the edge over about a screen pixel. Single-channel fields round sharp corners at large sizes; MSDF can replace them later without changing the API (decision 18).

As built (logos): `renderer/Image` decodes PNG and JPEG (stb_image) and makes single-channel distance fields from an image's alpha: trimmed to the artwork, downsampled with 3x oversampling, and computed with an exact Euclidean distance transform (Felzenszwalb). Pixels right at the edge use their coverage for sub-pixel precision; faint pixels far from an edge don't, so anti-aliasing noise in the source never becomes dots. An optional pass drops specks smaller than a fraction of the largest shape. The game loads the four logo images from `content/textures` this way at startup (`game/Brand`), so one texture per logo draws sharp at any size and in any tint. Textures from files for other uses (atlases, ASTC) are still to come.

### 10.4 Coordinate system and screen handling

- **Virtual resolution** for layout (for example 1080 x 1920 portrait or 1920 x 1080 landscape, per game), scaled to the actual display.
- **Safe area** from SDL (`SDL_GetWindowSafeArea`) is exposed to UI and playfield layout so nothing important sits under notches or rounded corners.
- Aspect ratios from 4:3 tablets up to 21:9 phones must be supported by every playfield layout.

As built (M4 menus): `Camera2D` fixes the shorter side at 1080 and lets the longer one follow the aspect ratio, so the canvas is 1920 x 1080 in 16:9 landscape and 1080 x 1920 in portrait. Layouts compare width and height to pick the orientation (decision 17). The safe area isn't applied yet.

As built (M4, branding and Android prep): `Platform::GetSafeArea` returns SDL's safe area as a fraction of the window, and `Ui::GetSafeRect` turns it into canvas units each frame. The song select, results, and the gameplay HUD (pause button, score) lay out inside it, while backgrounds still reach the screen's edges. The playfield's lanes still span the whole canvas. An Orientation setting (Auto, Landscape, Portrait) locks the screen: on Android through `Activity.setRequestedOrientation` (`USER_LANDSCAPE`, `USER_PORTRAIT`, `FULL_USER`, so either way up still works, as the rotation lock allows), and only when that's a change from what the activity has, since each request makes the system apply the rotation lock again and turns the screen about; `HyoshiActivity` switches off SDL's own requests (`setOrientationBis`), which came at every window creation, and on the desktop by turning the window to the matching shape, fitted to the display. `HYOSHI_WINDOW_SIZE` overrides it at startup.

### 10.5 GPU note positioning

Notes do not update positions on the CPU. Each mode uploads its note data once when a chart loads, and the vertex shader computes positions every frame from a few push constants.

**Scroll velocity (SV) handled with precomputed scroll positions:**

Instead of storing only a note's time, the chart loader precomputes each note's **scroll position**: the integral of scroll velocity from song start to the note's time. Each frame, the CPU computes the current scroll position once (a cursor walk through SV segments) and sends it as a push constant.

```
// Vertex shader (conceptual)
float distance = (note.ScrollPosition - push.CurrentScrollPosition) * push.ScrollSpeed;
float y = push.JudgeLineY - distance;
```

This makes speed changes, stops, and even reverse scrolling free on the GPU. Long notes store scroll positions for both head and tail.

For the Line mode, each judgment line has its own scroll integral and an animated transform. The CPU evaluates line transforms each frame (dozens of lines at most), uploads them as a small buffer, and notes reference their line by index.

### 10.6 Frame pacing

- Render at the display's refresh rate (60, 90, or 120 Hz), capped by FIFO present.
- Desktop games that ask for low latency (`AppConfig::LowLatency`, for a cursor the game draws) present with IMMEDIATE (MAILBOX where it isn't offered; MAILBOX alone still lagged in fullscreen on an NVIDIA card with G-SYNC) and cap the frame rate at four times the display's, waiting before the frame's events are read so it starts with the newest input.
- Never render uncapped on mobile.
- Android Frame Pacing library (Swappy) is a planned addition after M4 for smoother presentation on Android.
- On menus with no animation, drop to a low frame rate or render on demand to save battery.

---

## 11. Audio

### 11.1 Backend interface

```cpp
using SoundHandle = uint32_t;
using SoundId = uint64_t;   // hashed asset ID
using BusId = uint32_t;

constexpr SoundHandle INVALID_SOUND_HANDLE = 0;

enum class AudioCapability
{
    ScheduledPlayback,
    AdaptiveMusic,
    Snapshots,
    Spatialization
};

class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    virtual Result<void> Initialize(const AudioConfig& config) = 0;
    virtual void Shutdown() = 0;
    virtual void Update() = 0;
    virtual void Suspend() = 0;   // app backgrounded
    virtual void Resume() = 0;

    // Playback
    virtual SoundHandle Play(SoundId sound, BusId bus) = 0;
    virtual void Stop(SoundHandle handle) = 0;
    virtual void SetParameter(SoundHandle handle, uint32_t paramId, float value) = 0;
    virtual void SetBusVolume(BusId bus, float volume) = 0;

    // Timing contract for the rhythm core
    virtual AudioClockSnapshot GetClockSnapshot() const = 0;
    virtual int64_t GetEstimatedOutputLatencyUs() const = 0;
    virtual void ScheduleAt(SoundHandle handle, DspFrame frame) = 0;

    // Route changes (headphones, Bluetooth)
    virtual bool ConsumeRouteChangedFlag() = 0;

    virtual bool SupportsCapability(AudioCapability capability) const = 0;
};
```

### 11.2 miniaudio backend

| Topic | Decision |
|---|---|
| Device | Playback only, `f32` output, **device native sample rate** (avoid resampling latency) |
| Android | AAudio backend, `performanceProfile = ma_performance_profile_low_latency` |
| Apple | CoreAudio |
| Mixing | Custom mixing in the device callback (not `ma_engine`), so the engine knows exactly which frame is playing |
| Music (v1) | Fully decoded to 16-bit PCM in memory on a worker thread before the song starts. Simple, makes seeking trivial (important for the chart editor). A 3 minute stereo song is roughly 35 MB at 16-bit |
| Music (later) | Streaming decode into a ring buffer, if memory on low-end devices requires it |
| Hit sounds | Preloaded PCM, fixed voice pool (32 voices), sample-accurate scheduling via `ScheduleAt` |
| Buses | `Master`, `Music`, `HitSounds`, `Ui`, `Preview` |
| Route changes | miniaudio device notifications (`rerouted`) set an atomic flag; the game prompts the player to recalibrate |

As built (M3): `ScheduleAt(handle, DspFrame)` became `PlayAtMusicFrame(sound, bus, musicFrame)`, which starts a sound when the music reaches a frame of the song rather than of the device, so scheduled sounds follow pauses and seeks. A seek cancels scheduled sounds, and one whose frame has already passed when the audio thread receives it is dropped rather than played late. Music has its own calls (`SetMusic`, `PlayMusic`, `PauseMusic`, `SeekMusic`). `SetParameter` and `SupportsCapability` wait until something needs them.

As built (music rate): `SetMusicRate` plays the music faster or slower with its pitch following, as a tape does, from 0.1× to 4×, so an editor can preview a chart slowed down and still hear the beat. The mixer resamples linearly. Each frame rendered interpolates between the music frames on either side of a position that moves `rate` frames per frame. Streamed music arrives a block at a time, so on a block's last frame it holds that frame rather than look into the next block. That happens once every 2,048 frames and is inaudible. Frames stepped over past a block's end are skipped from the next one. Rate 1 keeps the exact copy it always had. Scheduled sounds start on the first frame rendered at or past their music frame, and play at their own speed. New music plays at 1, and `SongPlayer::SetMusicRate` reapplies its rate to music it loads later.

As built (music low-pass): `SetMusicLowPass(cutoffHz)` muffles the music, and only the music: two one-pole low-pass stages per channel run over the music the mixer has rendered, before the voices mix in, so sounds played over it stay clear. It's for a game's transitions, such as music behind a song that's loading. Off (0, or a cutoff past what the sample rate carries) it costs nothing, and new music plays without it, so whatever comes next starts clear. One test checks that it muffles the music, not a voice, until new music.

As built (streaming music): decoding whole songs up front was too slow to start a song or a song select preview: 120 to 540 ms per song in a release build on a fast desktop, over a second in a debug build, and 23 to 61 MB of PCM held per song. Music files now stream (`audio::MusicStream`, `SetMusicStream`), as most games do it; only sounds and generated tracks (the metronome) are decoded whole. Opening a stream reads the file and its headers and knows the length: about 7 to 25 ms for the same songs, file read included. A decoder thread per stream keeps 32 blocks of 2,048 frames (about 1.4 s, 256 KB) decoded ahead and passes them to the audio thread through two lock-free queues (decoded and free), so the callback stays real-time safe. Each seek starts an epoch: the backend sends the mixer's `SeekMusic` and the stream's `RequestSeek` the same epoch, blocks carry the epoch they were decoded for, and the mixer drops older blocks once it applies the seek. When no block is ready for the current position (right after a seek, or if the decoder falls behind), the mixer holds the music where it is and snapshots report it as not playing, so the song clock waits as it does for a pause instead of the music skipping ahead. After a seek, the music plays again once the audio thread has freed the old blocks and the first new one is decoded, an audio callback or two for WAV, FLAC, and Ogg Vorbis. MP3s go through the engine's own decoding backend (`audio/miniaudio/Mp3Backend.cpp`, miniaudio's MP3 decoder with its seek replaced), because miniaudio's MP3 seeks aren't exact. A seek-table seek starts decoding two frames back with the decoder cold, and the first of those frames usually lacks its bit reservoir, so it decodes to nothing and the seek lands one or two frames (1,152 samples each) late, varying through a file; and any seek but to the start takes its target to count the encoder delay, which reads leave out, so it also lands that much (1,105 samples for a typical LAME file) early. Measured on 29 MP3s, 200 positions each, against a full decode. A rhythm game can't have its music move against its chart after a seek, so MP3 streams first sought by decoding up to the target: exact, but 14 to 110 ms on average in a release build, and in a debug build up to 470 ms for a song select preview starting 2:22 in. The backend instead lists, at the first seek, every frame that decoding from the start produces, reading only headers and the bit reservoir (no samples; about a millisecond for a five-minute song, optimized, on an Apple Silicon Mac), and a seek decodes from 16 frames before the target's frame (the reservoir reaches back up to 511 bytes, up to 11 frames), the last two in full to settle the filters, then within the frame up to the target. Every position tested matched a full decode sample for sample at the file's own rate. A seek takes about 0.05 ms there (a scratch test built with -O2, not the release preset); in the game's debug build a preview's first block is ready 3 to 5 ms after its seek, wherever it starts. Ogg Vorbis seeks resampled to the device rate land within a frame (21 µs). `AudioStats::MusicUnderruns` counts the decoder falling behind mid-playback. A lead-in (`MusicStreamOptions::LeadInFrames`) is silence at the start of the stream, so games no longer copy the whole song to put silence in front of it.

As built (ASIO, Windows): `AudioConfig::AsioDriver` plays through an ASIO driver instead of the system's device (WASAPI, shared, 3 periods of 10 ms). `audio::AsioOutput` loads the driver as the COM object registered under `HKLM\SOFTWARE\ASIO`, opens its first two outputs at the driver's own sample rate and preferred buffer size, and fills them from the same `Mixer::Render` in the driver's buffer switch, converting to the driver's sample format (16, 24, or 32-bit integer, or float). The driver interface is declared from its documented binary layout rather than taken from Steinberg's SDK, whose licenses (its own, or the GPL) don't suit the engine's MIT license. The output latency is the driver's report (`getLatencies`). A driver that won't open falls back to the system's device, with the reason in `AudioDeviceInfo::FallbackReason`; a reset request from the driver (its buffer size changed in its control panel, `IAudioBackend::ShowOutputSettings`) makes the buffers again on the main thread (a message that only its latencies changed just reads them again, as the ASIO spec asks and PortAudio and RtAudio do, and tells song players through the route change flag; making the buffers again for it stopped the music meanwhile) (`Update`). A game chooses the driver before audio starts, in `Game::ConfigureAudio`, and can switch while it runs with `IAudioBackend::Reopen` (a game's Restart audio): the output closes and the named one opens under the same mixer, so every sound, the music, and its place carry on, at the rate audio started at. The system's device is opened at that rate (resampling to its own); an ASIO driver is asked to play at it (`AsioOutput::Open`'s wanted rate) and fails if it can't, as the mixer's sounds can't change rate. An output that won't open leaves the one before playing, with the reason in `FallbackReason`. Song players take the new output's latency through the route change flag. `HYOSHI_STRESS_REOPEN=1` reopens the output every 2 s.

### 11.3 Audio callback rules

Inside the callback, it is forbidden to: allocate memory, lock a mutex, log, touch files, or call into gameplay code. Commands (play, stop, volume) arrive through an `SpscQueue<AudioCommand>` drained at the start of each callback.

### 11.4 Latency estimation

1. **Baseline estimate:** buffer period size times period count, converted to microseconds. ASIO drivers report their own.
2. **Better estimate (planned):** on Android, AAudio timestamps (`AAudioStream_getTimestamp`) report the frame position actually presented at a given time, which gives true output latency. This may require reaching the underlying AAudio stream from miniaudio or a small native hook. Investigate during M3.
3. **Player calibration** covers whatever remains, especially Bluetooth, which frequently adds well over 100 ms.

### 11.5 Calibration flows

- **Audio offset test:** a steady metronome; the player taps along; the median tap error becomes `UserAudioOffsetUs`. Reject outliers.
- **Visual offset test:** a flashing indicator synced to beats; the player adjusts until sound and visual feel aligned.
- Offsets are stored per output route when possible (speaker, wired, Bluetooth device name), so switching headphones restores the right values.

As built (M4): no calibration scene yet. Instead, `rhythm::OffsetCalibration` keeps the player's last 100 hits (heads only, no misses), each with the offset it was made under, and suggests the offset that puts their median error at zero. The Mania settings panel applies it to the global offset or to the current chart's offset, then discards the hits. Offsets are saved in `settings.json` in the user data directory: one global audio offset (not yet per output route), a visual offset, and per-chart audio offsets keyed by artist, title, charter, and difficulty. Charts are never written to. `-` and `=` nudge the current chart's offset during play.

As built (M4 menus): the suggestion moved to the results screen. After a play with 20 hits or more whose median is 2 ms or more off, it offers the correction for all maps (the global offset, clamped to ±600 ms) or for this map only, from that play's hits alone.

---

## 12. Input

### 12.1 Input event

```cpp
enum class InputEventType
{
    KeyDown,
    KeyUp,
    TouchDown,
    TouchMove,
    TouchUp,
    PointerMove,
    AxisChange
};

struct InputEvent
{
    InputEventType Type;
    HostTimeNs HostTime;     // OS timestamp, converted to the host clock domain
    uint32_t DeviceId;
    uint64_t FingerId;       // touch only
    uint32_t Code;           // key code or axis ID
    float X;                 // normalized 0..1 (touch, pointer)
    float Y;
    float Value;             // axis value, pressure
};
```

### 12.2 Pipeline

```
OS event (with OS timestamp)
  -> SDL3 event (nanosecond timestamp)
  -> Platform layer converts to InputEvent in the host clock domain
  -> InputQueue (per frame, ordered by HostTime)
  -> Active mode's InputMapper turns raw events into GameplayActions
  -> Judge consumes GameplayActions in timestamp order
```

As built (M4): `InputQueue` (`engine/input`) collects the frame's events, and `Take` hands them over sorted by host time, stable so a press and release with one timestamp keep their order. Keys use SDL scancodes as codes, so bindings follow key positions rather than the keyboard layout. `PointerDown` and `PointerUp` joined the event types for mouse buttons; mouse events SDL synthesizes from touches are skipped, so a touch arrives once. There is no `InputMapper` yet: the Mania scene maps keys and touches to lane actions itself.

As built (M4 menus): `Wheel` events carry mouse wheel and trackpad scrolling. Key repeats reach the queue marked `IsRepeat`: menus use them to scroll a held arrow key, and gameplay ignores them.

As built (M8): Mac trackpads report each finger's position on the pad, and `TrackpadDown`, `TrackpadMove`, and `TrackpadUp` carry them (X and Y across the pad, not the window), so a game can aim with the pad as a tablet's pen aims: the pad maps onto the window. The OS moves the pointer from the same fingers, and menus ignore these events. `Platform::SetPointerLocked` hides the pointer and keeps it in the window while such a game plays, since AppKit sends the touches to the view under the pointer.

`input/Trackpad.h` maps them as tablet drivers do: a `TrackpadArea` (size, center, clockwise turn) is the part of the pad that covers the window, and `MapTrackpadToWindow` takes a finger there, clamped to the window. The pad's real size isn't known, so it's taken to have the window's shape, and the turn happens in square units. `TrackpadFingers` follows the fingers in the order they landed; the newest aims.

### 12.3 Timestamp accuracy (verify early)

SDL3 event timestamps are in nanoseconds, but **M1 must verify** that on Android they originate from the OS event time (`MotionEvent` event time) rather than the moment SDL processed the event. If they do not, add a thin native hook that captures `MotionEvent.getEventTime()` and historical samples. The same check applies to iOS (`UITouch.timestamp`) later.

As built (Windows): SDL 3.4 stamps Windows key and mouse messages from `GetMessageTime`, the system tick count, which advances in 15 to 16 ms steps. `timeBeginPeriod(1)`, which SDL calls, doesn't make it finer (measured on the development PC). Press times were therefore rounded to that step, as coarse as the ±16 ms Perfect window. Fixed for keys (2026-10-09): `SDL_HINT_WINDOWS_RAW_KEYBOARD` has SDL read them as raw input on its own thread, stamped with `SDL_GetTicksNS` as they arrive. Mouse buttons are raw only while the pointer is locked (`Platform::SetPointerLocked`, relative mode); otherwise they still carry the message time.

### 12.4 Touch handling

- Finger tracking by `FingerId` for holds and slides across lanes
- Hit zones defined in virtual playfield coordinates, with configurable padding
- Gesture recognition shared across modes: tap, hold, drag, flick (velocity threshold within a time window)
- Keyboard input kept on desktop for development and for desktop builds

### 12.5 Gameplay actions

Each mode defines its own actions. For example, Mania: `LanePress(lane)`, `LaneRelease(lane)`. Circle: `ClickPress(x, y)`, `CursorMove(x, y)`. Highway: `ButtonPress(id)`, `KnobDelta(side, value)`.

---

## 13. Chart Format

### 13.1 Two representations

| Form | Extension (placeholder) | Purpose |
|---|---|---|
| Source | `.rchart.json` | Human readable, diffable in git, written by the chart editor and importers |
| Cooked | `.rchart` | Compact binary, loaded at runtime, produced by the asset cooker |

### 13.2 Core schema

All times are integer microseconds relative to the start of the audio file.

```json
{
  "formatVersion": 1,
  "mode": "mania",
  "metadata": {
    "title": "Example Song",
    "artist": "Example Artist",
    "charter": "Britoshi",
    "difficultyName": "Hard",
    "difficultyValue": 7.5,
    "previewStartUs": 45000000,
    "pack": "Example Pack"
  },
  "audio": {
    "file": "audio/example.ogg",
    "offsetUs": 0
  },
  "timing": [
    { "timeUs": 0,        "bpm": 180.0, "meter": [4, 4] },
    { "timeUs": 64000000, "bpm": 90.0,  "meter": [4, 4] }
  ],
  "scrollVelocity": [
    { "timeUs": 0,        "multiplier": 1.0 },
    { "timeUs": 32000000, "multiplier": 0.5 }
  ],
  "modeSettings": {
    "laneCount": 4
  },
  "notes": [
    { "timeUs": 1000000, "lane": 0 },
    { "timeUs": 1333333, "lane": 2, "endTimeUs": 2000000 }
  ],
  "timeline": [
    {
      "target": "playfield.rotation",
      "keys": [
        { "timeUs": 16000000, "value": 0.0,  "ease": "linear" },
        { "timeUs": 17000000, "value": 15.0, "ease": "outCubic" }
      ]
    }
  ]
}
```

`pack` is optional: the collection of songs the chart belongs to, for a game's menus to group songs by (a medley names its pack the same way, at its root).

As built (M4): notes can carry their own sound (a keysound), for every mode. The root has an optional `samples` array of sound files relative to the chart, and a note refers to one with `"sample": <index>` and an optional `"sampleVolume"` (percent, default 100): `{ "timeUs": 1000000, "lane": 0, "sample": 0, "sampleVolume": 80 }`. The sound belongs to the note's time, not to hitting it; players choose whether they hear it. Charts without samples omit both, so the format version stays 1.

### 13.3 Mode-specific note payloads

| Mode | Note fields |
|---|---|
| Mania | `lane`, optional `endTimeUs` |
| Drum | `kind` (center or rim), `big` (bool), optional `endTimeUs` for rolls, `hitsRequired` for spinner-style notes |
| Circle | `x`, `y` (playfield units), optional slider: `path` (type, control points), `repeats`, `lengthUnits` |
| Highway | `lane` (buttons) or `side` + `points` (laser path: time and horizontal position pairs) |
| Line | `lineId`, `x` (position along the line), `side` (above or below), `kind` (tap, hold, drag, flick), optional `endTimeUs`, `speedMultiplier` (as built: `line` and `speed`, section 16.5) |

The Line mode also defines `lines` in `modeSettings`, each with its own timeline tracks (position, rotation, opacity, speed, and how it's drawn: length, thickness, and color).

### 13.4 Rules

- Notes are stored **sorted by time**. The loader validates this and rejects unsorted charts.
- The chart hash (xxHash of the cooked file) identifies a chart for replays and scores.
- `formatVersion` enables migrations; loaders must support all previous versions or refuse clearly.
- Importers for community formats (osu! `.osu`, StepMania `.sm`/`.ssc`) are planned as tools, useful for testing with real charts. Imported content is for local testing only unless the chart's author permits otherwise.

### 13.5 As built: several modes in one chart (format 2)

A chart can mix modes: format 2 lists them, and each note says which it belongs to. Format 1 charts read as before.

```json
{
  "formatVersion": 2,
  "modes": ["mania", "phigros"],
  "modeSettings": {
    "mania": { "laneCount": 4 },
    "phigros": { "lines": [{ "x": 0.5, "y": [{ "timeUs": 0, "value": 0.25 }, { "timeUs": 8000000, "value": 0.6 }], "speed": 1.2 }] }
  },
  "notes": [
    { "timeUs": 1000000, "mode": "mania", "lane": 0 },
    { "timeUs": 1000000, "mode": "phigros", "line": 0, "x": -2.5, "side": "above", "kind": "flick" }
  ]
}
```

- `modes` replaces `mode`: up to `Chart::MAX_MODES` (8) names, no repeats. `modeSettings` has an object per mode, keyed by its name. A note's `mode` is required when there's more than one and must be one listed; with one, it's that one. The rest of the chart (metadata, audio, timing, scroll velocity, samples) is shared.
- `ParseChart` takes a `ChartModeFormat` per mode the game knows (`ReadSettings` and `ReadNote` callbacks), and a chart listing a mode it wasn't given is refused. `ChartNote::Mode` is the note's index into `Chart::Modes`, and `SelectMode` copies the chart with one mode's notes, to build that mode's runtime chart from.
- The modes so far: `mania` (osu!mania's lanes, `ManiaJudge`), `arcaea` (the same lane fields, Arcaea's rules: section 16.6), and `phigros` (the Line mode: section 16.5). `mania::MakeLaneFormat(name, ...)` reads a lane mode under any name. A game decides what plays together (one lane mode with Phigros lines over it, say).
- Notes are sorted by time across all modes, as in format 1. Nothing converts notes from one mode to another.
- `WriteChart(chart, modes)` writes format 2 with the same formats (their `WriteSettings` and `WriteNote`; `ChartFieldWriter::AddObject` and `AddObjects` write nested settings such as lines and their keys). A note's `WriteNote` gets its place among its mode's notes. Values with one key at the start are written as numbers, and floats as the shortest decimal that reads back the same.

### 13.6 As built: StepMania charts

`mania::ImportStepMania` reads StepMania's `.sm` and `.ssc` files into Mania charts, one per chart in the file, a lane for each column (dance-single 4, dance-double 8, pump-single 5, and so on, up to `MAX_LANES`).

- **Tags.** `#KEY:value;`, a value running over lines until its semicolon (or a line that starts the next tag); `//` comments. In `.ssc`, a chart's tags follow its `#NOTEDATA`, and its own music and credit win over the song's; a chart with its own `#BPMS` has its own timing (split timing). In `.sm`, `#NOTES` holds type, description, difficulty, meter, radar values, and notes, separated by colons.
- **Notes.** Measures of 4 beats, comma-separated, each split evenly into its rows, a character per column: `1` a tap, `2` and `4` a hold's and a roll's head (rolls are holds), `3` their tail, `L` a lift (a tap). Mines, fakes, and the rest are left out, with a warning.
- **Time.** Beat 0 is at -`#OFFSET` seconds; `#BPMS` (beat=BPM) set the pace, `#STOPS` (beat=seconds) pause after that beat's notes and `#DELAYS` before them, and `#WARPS` (beat=beats) skip ahead, their notes left out. A non-positive BPM is skipped over like a warp, as old charts warp with negative BPMs. Notes scroll at one speed (`#SCROLLS` and `#SPEEDS` are left out). The BPM changes become timing points.
- **Names.** Title `#TITLE` and `#SUBTITLE`, artist `#ARTIST`, charter the chart's `#CREDIT` (else the song's), difficulty name its difficulty and meter ("Challenge 12"; other chart types put their type first; Edit charts add their description; repeats get a number), preview `#SAMPLESTART`, background `#BACKGROUND`.
- **In the library.** A file's charts are listed apart: `songs::MakeStepManiaChartPath` names each by its place after a `#` ("Song.ssc#2"; the file alone is the first), which `LoadMixedChartFile` reads, and `ReadManiaChart` returns one entry per chart (a `ChartReader` returns every chart in a file). A folder with both `.ssc` and `.sm` lists the `.ssc`. Song folders are scanned three folders deep, for StepMania's packs (pack, then song folders). A pack comes as a `.zip`, which `IsMapArchive` takes; an archive whose files all sit in one folder unpacks that folder's contents.
- **Opened files.** `AppConfig::OpenedFiles` (a game's command-line arguments, as a desktop's Open with passes a file) arrive as dropped files; on Android, the activity copies files opened with the app (section 7).


### 13.7 As built: medleys

A `.medley.json` (`songs::LoadMedleyFile`) plays one song in parts from other charts of it, each as its own mode, on one audio file: its sources (any chart `LoadMixedChartFile` reads, or an osu!standard `.osu` for circles) each move by `audioLagMs` (where its own audio starts in the medley's) plus its chart offset (or `offsetMs` in its place) less the medley's, and keep only their notes in their parts (a note belongs to the part its start falls in; holds and sliders run on). A source's lanes can play as the other lane mode's (`lanesAs`: a StepMania chart on Arcaea's track, say). Lanes are rebuilt from the kept notes (`BuildManiaChart`), lines with every key moved (`BuildLineChart`; floor positions only shift by a constant), circles with their notes and sample points moved and finished again (`FinishCircleChart`), a note after a cut starting a new combo. A part can name several sources, which play together (their modes the part's). One source each may have lanes and lines; several may have circles, each kept to its parts and merged (`MergeCircles`: control points and sounds renumbered, sample points kept to each source's parts) under the first's difficulty and colors. Notes of different modes due together are marked to shine (`MarkSimultaneousAcrossModes`, for any chart mixing modes). The result is a `LoadedMixedChart` with `Circles` and `Parts` (each part's modes and span), which a game plays as it likes; the library lists a folder's medleys and not their sources.

### 13.8 As built: kiai time

`Chart::Kiai` holds a chart's highlights, osu!'s kiai time: sorted sections, apart, for a game's effects (none for formats without it). Both osu! importers read it from every timing point's effects (the eighth field, bit 0), red and green lines alike: on from a point that turns it on to the next that doesn't, the last of several points at one time counting (as osu!lazer's replace each other), and a section still on at the end running to the last note's end (`osu::detail::MakeKiaiSections`). A medley's is every source's, moved onto its clock and kept to the parts that source plays, joined where they meet. The cooked JSON format doesn't carry it yet.

---

## 14. Rhythm Core

### 14.1 Runtime chart

At load, the source or cooked chart becomes packed arrays:

```cpp
struct NoteRuntime
{
    SongTimeUs Time;
    SongTimeUs EndTime;          // equals Time for non-hold notes
    double ScrollPosition;       // precomputed SV integral at Time
    double EndScrollPosition;
    uint32_t ModeDataIndex;      // index into the mode's own payload array
    uint8_t State;               // Pending, Active, Hit, Missed
};
```

### 14.2 Note cursor

A `NoteCursor` walks the time-sorted array. Per frame it only inspects notes in the window `[songTime - maxMissWindow, songTime + lookAhead]`. Anything older than the miss window becomes a miss automatically.

### 14.3 Judgment framework

Shared framework, mode-specific rules:

```cpp
enum class Judgment
{
    Perfect,
    Great,
    Good,
    Bad,
    Miss
};

struct HitWindows
{
    int64_t PerfectUs = 16000;
    int64_t GreatUs = 40000;
    int64_t GoodUs = 73000;
    int64_t BadUs = 103000;
};

struct JudgmentEvent
{
    uint32_t NoteIndex;
    Judgment Result;
    int64_t ErrorUs;             // negative = early, positive = late
    SongTimeUs Time;
};

class IJudge
{
public:
    virtual ~IJudge() = default;

    virtual void Reset(const ChartRuntime& chart) = 0;
    virtual void ProcessAction(const GameplayAction& action, SongTimeUs actionTime,
                               JudgmentSink& sink) = 0;
    virtual void Advance(SongTimeUs songTime, JudgmentSink& sink) = 0;  // auto misses, hold ticks
};
```

The window values above are **starting points to tune**, not final numbers.

### 14.4 Scoring and combo

`ScoreSystem` consumes `JudgmentEvent`s and produces score, combo, accuracy, and a judgment histogram. Scoring formulas are per game (configurable), not hard-coded into the engine.

### 14.5 Determinism rules

1. Judgment consumes only: the chart, `GameplayAction`s with `SongTimeUs` timestamps, and mode settings.
2. No floating point in time comparisons. Positions (touch, cursor) are quantized when recorded so replays reproduce them exactly.
3. No frame delta, no random numbers (or seeded RNG stored in the replay).
4. Actions are processed strictly in timestamp order.

### 14.6 Replays

```cpp
struct ReplayHeader
{
    uint32_t FormatVersion;
    uint64_t ChartHash;
    uint32_t ModeId;
    uint32_t EngineVersion;
    uint64_t RngSeed;
    int64_t AudioOffsetUs;
    int64_t VisualOffsetUs;
    // mods, settings...
};
```

Body: the list of `GameplayAction`s with song times. Replaying means feeding the same actions into a fresh `IJudge`. Replays serve as:

- Player-facing replays
- **Regression tests** (Section 23): a replay plus its expected final score must always match
- A future foundation for anti-cheat validation

### As built (M4)

- **No `IJudge` yet.** `ManiaJudge` (`modes/mania`) is a concrete class; the interface waits for the second mode (M6). The sink is a `std::vector<JudgmentEvent>&`, and each action carries its own song time. `JudgmentEvent` gained `IsTail` for hold ends.
- **Mania rules.** A press judges the earliest pending note in its lane when inside the Bad window; a press too early for it does nothing. A note is missed one microsecond after its Bad window closes. A hold held to its end completes as a Perfect tail at its end time. Released early, the release is judged against the end time with the same windows, and a release before the Bad window is a tail Miss. A missed head also misses its tail. Windows are inclusive.
- **Order.** `Advance` judges due misses and hold completions across all lanes in time order (ties by lane), and `ProcessAction` advances to the action's time first, so the events don't depend on how the time between actions is split into frames.
- **Live play is a replay.** The scene converts each input's host timestamp with `HostTimeToSongTime`, raises it to the judge's current time if older, and records it. Each frame advances the judge to 20 ms behind the current song time, so input that arrives a frame late is still judged by its timestamp. Feeding the recorded actions to a fresh judge gives the same events; a unit test checks this on a 1,000-note chart.
- **Scoring.** Integer only: 300, 200, 100, 50, and 0 points; Bad and Miss break combo. Score is 1,000,000 × points / (300 × judgments), and accuracy is in basis points. A hold counts twice, head and tail.
- **Autoplay** turns note times into host timestamps with `SongTimeToHostTime`, the exact inverse of the filtered mapping, and sends them through the same path as key presses. The scripted check fails the run if autoplay ends below the maximum score.

---

## 15. Timeline Animation System

Needed heavily by the Line mode (moving, rotating judgment lines) and the Highway mode (camera tilt, zoom), and useful everywhere (beat-synced UI pulses, background effects).

### 15.1 Model

```cpp
enum class Ease
{
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    InSine,
    OutSine,
    InOutSine,
    Step
    // more as needed
};

template <typename T>
struct Keyframe
{
    SongTimeUs Time;
    T Value;
    Ease EaseToNext;
};

template <typename T>
class Track
{
public:
    T Evaluate(SongTimeUs time);   // uses a cached cursor, O(1) for forward playback

private:
    std::vector<Keyframe<T>> keys;
    size_t cursor = 0;
};
```

### 15.2 Binding

Tracks bind to named targets (`"line.3.rotation"`, `"camera.tilt"`, `"playfield.scale"`). Modes register which targets they expose. The binding is resolved once at chart load into direct pointers or indices, so evaluation never does string lookups.

### 15.3 Evaluation

Tracks evaluate at the smoothed song time once per frame, before rendering. Seeking backwards resets cursors (binary search to the new position).

---

## 16. Game Modes

### 16.1 Mode interface

```cpp
class IGameMode
{
public:
    virtual ~IGameMode() = default;

    virtual const char* GetName() const = 0;
    virtual Result<void> LoadChartData(const ChartSource& source, ChartRuntime& outChart) = 0;
    virtual std::unique_ptr<IInputMapper> CreateInputMapper(const ModeSettings& settings) = 0;
    virtual std::unique_ptr<IJudge> CreateJudge(const ModeSettings& settings) = 0;
    virtual std::unique_ptr<IPlayfieldRenderer> CreatePlayfieldRenderer(IRenderDevice& device) = 0;
    virtual void RegisterTimelineTargets(TimelineRegistry& registry) = 0;
};
```

Modes register themselves with a `ModeRegistry` at startup. The gameplay scene asks the registry for the mode named in the chart.

### 16.2 Mode summaries

| Mode | Input (mobile) | Rendering | Special systems |
|---|---|---|---|
| **Mania** | Touch lanes at the bottom of the screen | 2D lanes, GPU-positioned notes, long note bodies; as built, also a track in perspective (`PlayfieldView::Track`, below) | SV via scroll positions |
| **Drum** | Tap zones: center vs rim (or left and right halves) | Single horizontal lane scrolling | Roll and spinner-style notes with hit counts |
| **Circle** | Direct touch on circles; slider follow with finger | Circles with approach rings (fade in, no scrolling), slider bodies | Slider curve evaluation (linear, Bezier, circular arc, Catmull-Rom); slider body mesh generation; cursor/finger tracking |
| **Highway** | Touch buttons plus two drag zones as virtual knobs; gamepad on desktop | Perspective highway (Camera3D + depth), laser ribbons | Analog judgment over time; camera timeline tracks |
| **Line** | Free touch anywhere near notes on lines; flicks, drags, holds | Rotating, moving lines with notes attached | Heavy timeline animation; per-line scroll integrals; gesture recognition |

`PlayfieldStyle::LaneColors` gives the lanes a map's own colors, mirrored from the outside in (the outer lanes the first), with the track's edges in the first; empty, the usual white and blue with a gold middle lane.

As built (Mania's track view): `PlayfieldStyle::View = PlayfieldView::Track` lays the same lanes on a floor receding up the screen, as in Arcaea, drawn with `SpriteBatch::DrawQuad` from a pinhole projection on the CPU (no depth buffer or Camera3D yet). A point `along` the lanes (scroll units past the judgment line) is drawn at eye / (eye + along) of its size at the judgment line, where the lanes are `LaneWidth` wide, so the judgment line, judging, and layouts that only use it are as on a flat playfield. The track vanishes 8% down the canvas, the eye is 1.15 judgment-line-to-horizon heights behind the line, and the track ends where it's drawn at 0.2 scale, where notes appear. Notes and hold ends are stretched along the track as the scroll is, so they're as long in time (and as thick against their gaps) as on flat lanes. A note appearing flares up and settles over the first 35% of the way: a glow that shrinks onto it, a beam of light in its lane's color rising from it and fading upward, and its lane's end of the track lighting up. The judgment banner shrinks to the track's width at its height. `ManiaPlayfield::GetJudgeLineX` maps a canvas point to where the lane under it meets the judgment line, for touch input on the track.

### 16.3 Build order

1. **Mania** validates the entire core.
2. **Drum** proves modes are pluggable with minimal new work.
3. **Circle** breaks the lane assumption; adds positional judgment and curves.
4. **Highway** adds perspective and analog input.
5. **Line** exercises everything, especially the timeline and gesture systems.

### 16.4 As built: Circle (M8, started ahead of Drum)

Circle came before Drum because the first game to use it was ready for it. It follows osu!standard, so real .osu maps play as they do there; where it differs, it says so below.

- **No `IGameMode` yet.** Like Mania, the mode is concrete classes in `modes/circle`: `CircleChart` (with its import and derived data), `CircleJudge` (+ autoplay), `CircleScore`, and `CirclePlayfield` (drawing). A registry waits until a game needs to pick modes at run time.
- **Chart.** Positions are osu! playfield units, 512 by 384, y down, whatever the screen, and judging happens in them, so replays play the same at any resolution. From the difficulty settings: radius 54.4 − 4.48 × CS; approach 1800 − 120 × AR ms below AR 5, 1200 at 5, 1200 − 150 × (AR − 5) above; windows (Great, Good, Bad: osu!'s 300, 100, 50) of 80 − 6 × OD, 140 − 8 × OD, and 200 − 10 × OD ms, inclusive.
- **Import** (`tools/osu-import`, `ImportOsuStandard`; `songs::LoadCircleChartFile` in memory). Circles, sliders, and spinners with new combos; the first note and the note after a spinner always start a combo. Slider duration is length / (SliderMultiplier × 100 × velocity) beats at the tempo in effect, times the passes. Green lines set the velocity to −100 / beatLength, clamped to 0.1 to 10, and red lines reset it to 1. A missing ApproachRate takes OverallDifficulty (old files). Files before format v5 get osu!'s 24 ms. A slider with one control point becomes a circle, and a curve type change inside a slider (lazer's newer format) is read as the first type; both warn. osu!mania holds in a standard map are skipped. Slider ticks come every SliderMultiplier × 100 × velocity / SliderTickRate units along the path (without the velocity before format v8, as osu! did). Every timing line, red or green, becomes a sample point (bank, custom sample index, volume; a sample set of 0 takes [General] SampleSet), and each note's hit sound bits and hitSample field become its `HitSound`, with what it leaves at 0 taken from the sample point 5 ms after it, as osu! looks it up; a slider gets one for its body (its ticks') and one for each edge, from edgeSounds and edgeSets. The command-line tool still converts osu!mania only.
- **Slider paths** are sampled once, at load: Bezier (split where a control point repeats), linear, Catmull-Rom, and circular arcs (exactly three points, a straight line if they're collinear; otherwise Bezier). The dense curve is resampled to points evenly spaced along the slider's length, at most a quarter radius apart (8 to 256 per slider), clipped to that length. Drawing and the ball both use these points.
- **Stacking** is lazer's `applyStacking` over the whole chart: notes within 3 units of each other and closer in time than approach × StackLeniency stack, each level moving the note 6.4 × scale up and left, with scale = (1 − 0.7 × (CS − 5) / 5) / 2. A slider stacks by the end of its path, whatever its passes. Spinners take no part, nor do gliding notes unless `CircleDifficulty::AreGlidingNotesStacked` (they come in from elsewhere, and the offset would move their whole path); leniency 0 turns it off.
- **Judge** (lazer's `StartTimeOrderedHitPolicy`). A press strikes the earliest pending note under the pointer whose Bad window holds it, in time order, never the nearest: a circle, or a slider's head. Note lock: a press that would strike a note while an earlier circle is pending does nothing if it comes before that circle's time; after it, the press strikes and every earlier pending circle is missed. Notes within 1 ms are a chord and neither blocks nor misses the other. Only pressed circles block, and only they are missed by a later strike. A note not struck by the end of its Bad window is missed, stamped one microsecond after the window, however late it was noticed.
- **Slides and flicks** (`CircleNote::Head`, `HeadKind`). Besides pressed heads, a circle's or a slider's head can be a slide or a flick, struck without a press, as Phigros's drags and flicks are; both are Greats, untimed (`IsTimed` false, no error), since when a resting finger or a sweep happens to count isn't the player's choice. A press passes them by. A slide is struck by a pointer down on it from its time to the end of its Bad window: when one presses (unless the press struck or was blocked), moves, or touches there, or at its time for one already there (a check of its own, like a miss). A flick is struck by a pointer, down or not, moving across it within its Bad window either side at its speed or faster (`FlickSpeed`, playfield widths a second, or `CircleDifficulty::FlickSpeed`, 2 by default): across means the step from its last place comes within a radius, so a fast swipe can't skip it between actions, and its speed is how far it went over the last 50 ms (`FLICK_SPEED_WINDOW_US`), the pointer staying where it last was between actions, once it's been tracked for half that since it last let go (a finger that just landed is no flick, nor a mouse just let go). Note lock blocks a flick as it does a press; neither kind blocks, and a later strike doesn't miss them, so each keeps its window (a slide's might not have opened yet). Each action strikes every slide and flick it reaches, in time order.
- **Gliding notes.** A circle can glide in along a path (`CircleChart::GlidePath`, from `MakeGlidePath`), landing where it's struck at its time, instead of appearing there; judging is untouched, at where it lands. The path is 64 points evenly spaced along it (`GLIDE_PATH_POINTS`), from where it comes in to where it lands, so it flies at an even speed: `MakeGlidePath` takes the landing point and a curve out from it (sampled as slider paths are), cut short or carried on straight the way it ends to the length asked for. It sets off at `GlideStartTime`, and appears at `GlideAppearTime`, waiting at the path's start until it sets off, so an approach circle that opens first has a note to ride; it fades in over `GlideFadeUs`, or not at all. `GetHeadPositionAt` says where a note is drawn at a time, and `GetAppearTime` when it shows. Every note's approach circle can be its own (`ApproachLeadUs`, 0 for the approach time or negative for none, and `ApproachFadeUs`, 0 for 40% of the lead), never showing before its note.
- **Sliders** (osu!lazer's). A slider's parts (`CircleChart::SliderParts`, from `FinishCircleChart`, after lazer's `SliderEventGenerator`) are its head, a tick every tick distance along each pass (none within 10 ms of travel of a pass's end; met in reverse on the way back), a repeat where the ball turns, and the tail. Each is an event of its own (`JudgmentEvent::Part`, from 1), and the slider has no result of its own. The head is graded by timing, as a circle is (Great, Good, Bad, or a Miss at the end of its window; timed, so it counts for the offset and the histogram). The rest are hit or missed when the ball reaches them, by whether the slider is being followed then, but only once the head has been judged: a head struck late judges the parts the ball has already passed (lazer's `PostProcessHeadJudgement`), each hit while the press was within 2.4 radii of it, until one is out of reach, which misses it and the rest; after a missed head, they're judged by following at once. The tail is judged 36 ms early (or halfway through the slider, if that's later), as osu! does, since staying on to the very end is hard. A slider is followed while a pointer is down within 2.4 radii of its ball, once one has been down within one radius of it since it started; the head needn't be struck, so holding through a missed head still collects the rest. Following is checked at each action and at each part, in time order, so it stays deterministic. lazer also keeps a head struck with one key from being followed by another key held from before; here a pointer's buttons and keys are one, so it doesn't.
- **Where Circle differs from osu! (for now).** Spinners aren't judged. Slider bodies don't play osu!'s slide sounds. There's no HP, so no failing, and no mods.
- **Actions.** `CircleAction` Press, Release, Move, and Touch (a pointer down without a press, for a game mixing modes: a finger down for another mode's note, which follows sliders and strikes nothing) in `GameplayAction::Type`, with the pointer in `Index` (a finger, or the mouse with its buttons and keys as one pointer) and the position in 1/64 playfield units. Each press on a pointer strikes, even while another button of it is held, as alternating keys do in osu!. Autoplay gives each note the lowest pointer not busy with another, so chords and notes during a slider work, follows sliders with Moves every 16 ms and onto the ball at each part, and lets go 30 ms after a circle, or when a slider ends. A slide gets a Touch rather than a Press (so it strikes no other note there), and a flick a sweep: Moves from outside it 30 ms before (at 1.5 times its speed, from at least 1.5 radii out, on the way from the note before) to on it at its time, and on, letting go after (a slider's touches down there to follow it).
- **Hit sounds** (`songs::CircleHitSounds`). Each circle and slider part sounds its bank's normal sound with its additions (whistle, finish, clap) in the addition bank, at its volume, or only the file its hitSample names; a tick sounds its bank's slider tick. Files are osu!'s names (`soft-hitclap2.wav`), as .wav, .ogg, or .mp3, in any case: the chart's folder first for a custom sample index of 1 or more (the indexed name, then the plain one), then a skin folder the game gives, then sounds generated in place of osu!'s defaults, which the engine can't ship. A file that's there but empty or unreadable is silence, as maps mute sounds with one. Only what's hit sounds. A circle or a slider's head plays the moment it's struck. The other parts are judged after the ball passes them (the judge runs behind the clock by the input guard), too late to sound on time, so each is scheduled on its music frame just before the audio thread gets there (two callbacks past its last cursor) if its slider is being followed then; a part that's missed after all is stopped, and one hit without being scheduled plays at once (or on its frame, for a tail judged early). A part's frame in play is where a struck one would sound: the frame the music is at when a perfectly timed hit lands (`SongClock::MusicFrameForSongTime`, past the output latency and the player's audio offset), not the part's own frame, which sounded that much earlier than the player's own hits. Autoplay schedules everything ahead, on the parts' own frames, with the music. `LoadUnified` plays one sound the game gives instead, for every circle and slider part but ticks, at full volume, by the same rules.
- **Determinism.** As in Mania: `ProcessAction` advances to the action's time first, live play clamps action times to the judge's and records them, and a unit test plays sloppy actions live at three frame rates and as a replay and gets the same events.
- **Scoring** (`CircleScore`, osu!lazer's standardised score). Every judgment has a base value: circles and slider heads 300, 100, or 50, ticks and repeats 30, tails 150, misses 0. Accuracy is the base values earned over the most there were to earn, so far, ticks and tails included. Each hit adds one to the combo (the full combo is `CircleChart::GetMaxCombo`); a miss breaks it, except a missed tail, which only doesn't add. The score is 700,000 × the combo portion (each judgment's base value × √ the combo after it, over the same for a perfect run, its judgments in time order) + 300,000 × accuracy¹⁰ × the fraction of the chart's judgments made: 1,000,000 for a perfect run. It's computed in doubles in a fixed order with only exactly rounded operations (square roots, products, sums), so replays reproduce it. Ranks: SS at 100%, S at 95% without a miss, then A, B, and C at 90%, 80%, and 70%, else D. The 300s, 100s, 50s, and misses are the circles' and heads'. A game can judge and score its own way: `CircleJudge::Reset` takes windows to use instead of OD's (Phigros's, say: a Great within 80 ms, a Good within 160, and no Bad by making MehUs the Good window), `CircleScore::Reset` takes the head values (`CircleHeadValues`, a Good worth 195 for 65%), and `CirclePlayfieldStyle::MarkTexts` names the marks.
- **Notes due together shine** in every mode alike: a chart's build (`BuildManiaChart`, `BuildLineChart`, `FinishCircleChart`) marks each note due within `rhythm::SIMULTANEOUS_US` (1 ms) of another (`IsSimultaneous`, by `rhythm::MarkSimultaneous`: a lane chord, Phigros notes at one time on any lines, as Phigros highlights them, circles at one time), and the playfields draw `renderer::SHINE` (gold) around them: a frame around a lane note (on the track too) or a Phigros note, a glow behind a circle. Not across modes: a chart mixing them marks each mode's own.
- **Combo colors.** The importer reads a map's `[Colours]` (`Combo1` on, into `CircleChart::ComboColors`) and each new combo's color skip (type bits 4 to 6, `CircleNote::ComboColorSkip`); `FinishCircleChart` gives each note its combo's color (`ComboColor`: the first combo the first color, each new combo the next past its skip). With `CirclePlayfieldStyle::AreComboColorsShown` (on by default) and colors, a circle is its color with a light rim over a dark halo, its approach circle its color toward white over a dark ring, a slider body its color darkened inside a light rim, and the ball and the pop its color; colors too dark to read are lightened (`renderer::ToLightColor`). The rim and halo keep notes readable on any background, light or dark. Without colors, the usual gray.
- **Drawing** (`CirclePlayfield`). Notes fade in over the first 40% of their approach, and an unstruck one fades out over 0.2 s after its time; a struck one is replaced by a pop (growing by half as it fades, 0.1 s), and its result gets a mark (0.5 s; a slider's head's, at the head). Approach circles shrink from 3× to 1×. Slides and flicks are diamond outlines over a dark fill, yellow and orange whatever their combo's color, with approach circles of their color, slides 0.78 of a note's size; their pops are diamonds too. A gliding note is drawn along its path, its approach circle riding with it, and its pop and mark where it was struck. Slider bodies snake in between 1/3 and 2/3 of the approach, carry a ball (in a follow circle 2.4 radii across while it's followed), and shrink behind it on their last pass, with reverse arrows (outlined, and drawn over the notes, the slider's own head too, so nothing hides them; the start's once the ball has left it) and dots for the ticks the ball has yet to reach on its pass. Follow points are dashes every 32 units between consecutive steps of a combo, each appearing 0.8 s before its moment; into a chord the line forks toward its notes, and out of one the lines merge. Spinners are a ring that empties over their length. `CirclePlayfieldStyle::IsEditorView` draws slider bodies whole from the start, without snaking in or shrinking behind the ball, for an editor that shows a chart at any time. The shapes are generated at load (one texture), not skinned. Placement is osu!'s: the playfield takes 80% of a 640 by 480 screen fit to the canvas, 8 units low (`CircleLayout::Fit`).
- **Painter's order.** Notes are drawn latest first so the next one is on top, each with its combo number on it. That needs sprites of two textures (the shapes and the glyph pages) to interleave in one layer, so `SpriteBatch::KeepOrder(layer)` makes a layer draw in submission order, at a draw call per texture change. Slider bodies go in one such layer under all the circles, which go in another. The playfield uses five layers from `CirclePlayfieldStyle::FirstLayer`.
- **Opaque slider bodies.** A body is overlapping discs, the border color then the fill color smaller on top. Translucent discs would darken where they overlap, so bodies are opaque and appear at full strength (under their fading heads). Translucent bodies, and fading them, wait for render targets.
- **Lead-in.** A chart that starts soon after its music needs silence first. The game streams the music with that much silence in front (`SongPlayer::LoadFile`'s lead-in) and adds its length to `SongClock::ChartOffsetUs`, so note times keep counting from the audio file's start and song time is negative during the lead-in. The judge and the playfield take negative times.
- **Song library.** `LibraryOptions` picks the chart reader (`ReadManiaChart` by default, or `ReadCircleChart`) and whether the built-in Mania chart is listed. Readers run on the scan's worker, so they're plain functions. `LibraryChart::BackgroundPath` carries a chart's background image, `Tags` words to search by (an osu! map's tags and source, a StepMania song's genre), and `Pack` the first folder under the song folder it's in (its map set's or StepMania pack's). The engine's readers read only what a song list shows of the two commonest files, not building their charts: `osu::ReadOsuInfo` reads an .osu file's sections and counts its hit objects, and `mania::ReadStepManiaInfo` reads a StepMania file once for all its charts' details and notes' times (before, each chart re-read the whole file, and every chart was built). A game loads a chart in full only to play it. The game's own files, Phigros packages, and medleys are still loaded to be listed; they're few. On 147 songs (705 charts, 121 StepMania files and 101 .osu), a scan went from 19.6 s to 4.7 s in a debug build and from 1.5 s to 0.6 s in a release build.

### 16.5 As built: Line (Phigros)

The Line mode follows Phigros, in its own units, and is read from charts as the `phigros` mode (`modes/line`).

- **Field.** 1920 by 1080 units (`FIELD_WIDTH`, `FIELD_HEIGHT`), fit to the canvas by `LineLayout::Fit`, y down. A game can reshape it to the screen (`LineChart::FieldSize`, from `GetScreenFieldSize`: the screen's shape at the scale the usual field fits at, so a squarer screen's field is taller and a wider one's wider), as Phigros fills any screen: lines, at fractions of the field, spread over the whole screen, notes and the usual line stroke keep their size, and pictures and thick bars (a backdrop, a fill) stretch with it along and across their line, so what covered the field still covers the screen (round pictures go a little oval on a screen far from 16:9). The judge and autoplay use the same field. A line is placed by fractions of the field with y from the bottom and turned counterclockwise in degrees, as Phigros does. A note's `x` is along its line from the line's middle in `X_UNIT`s (0.05625 of the width, so the field is 17.8 across), and its distance from the line is in `DISTANCE_UNIT`s (0.6 of the height).
- **Lines.** Each of x, y, rotation, opacity, speed, length, thickness, red, green, and blue is a number or a list of `{timeUs, value}` keys: linear between keys, held before the first and after the last. Speed is held from key to key instead, and integrated over seconds into the line's floor position (`GetFloorPosition`). A note is drawn its floor position minus the line's, times its own `speed`, from the line, on its side; a hold's body runs to its end's floor position.
- **Windows by the game.** `rhythm::ChartMetadata::OverallDifficulty` keeps osu!'s OD from an osu!mania import (and the chart file's `metadata.overallDifficulty`), and `mania::GetOsuManiaWindows` turns it into osu!mania's windows (MAX 16 ms; 300, 200, and 100 at 64, 97, and 127 ms less 3 per OD; no 50s), for a game that judges that way. The judges take any windows: a game can give Phigros's Perfect and Good to every mode by making Great as wide as Perfect and Bad as wide as Good. The track view (`PlayfieldView::Track`) runs to 4% from the top of the canvas, a tenth as wide there, and `ManiaPlayfield::GetTrackScaleAt` says how much wider or narrower it is at a height than at the judgment line, so a game can size the lanes to fill the screen at its bottom; `PlayfieldStyle::ErrorMeterRangeUs` sets the timing meter's ends.
- **Judge** (`LineJudge`, after Phigros). A press reaches a note within 1.6 X units of it along its line (`HIT_HALF_WIDTH`), however far off the line, wherever the line is at the judge's time. Taps and hold heads are graded by timing: Perfect within 80 ms, Good within 160, and Bad from 160 to 180 ms early, the earliest note in reach first. A hold is then held to its end, a second event with the head's result; letting go or straying more than 1.5 reaches from it before its last 100 ms misses the end, unless another pointer down on it takes it over. Any pointer down counts as holding, however long it has been down: a drag is caught by a pointer down in reach at any moment within 160 ms of its time, struck or not, and a hold by one in reach once it has arrived, up to 160 ms after (Perfect, untimed, as if struck on time). Pointers that don't move are checked every 10 ms (`SWEEP_US`) while something can be caught, since lines move under still fingers (before, a drag was checked once as its window opened, and then only when a pointer moved, so a still finger missed drags its line brought to it). A flick is caught by a swipe across it then (a move of 1 field unit per ms or faster), each move catching only the flick nearest in time; both are Perfect, judged at their time or when caught after it. A press with a drag or flick in reach due nearer in time than the tap or hold it would strike is for that note and strikes nothing, as Phira gives a touch to the note nearest in time: otherwise a press for a flick strikes a tap just after it early. A key reaches every note as if pressed on it, and flicks. What isn't struck or caught by 160 ms after its time is missed. Actions are `LineAction` Press, Move, Release, KeyPress, KeyRelease, and Touch (a pointer down that strikes nothing, for a finger whose press went to another mode: it still catches and holds), with the pointer in `Index` and field positions in quarter units. `GetPressTarget` says when the note a press would strike is due, so a game mixing modes can give a touch to the mode whose note comes first; `GetStrikeTarget` the same for taps and holds alone (a drag or flick needs no press). `CircleJudge::GetPressTarget` does the same for circles. Autoplay (`MakeAutoplayActions`) presses, drags, and swipes each note on its line, pressing drags 8 ms early and starting a flick's swipe 30 ms early.
- **Drawing** (`LinePlayfield`, three layers: `Draw` is `DrawLines` in the first and `DrawNotes` in the other two, which a game can call apart to keep the lines up as a picture under something else while hiding the notes; `noteSpeed` scales notes' distance from their lines, a player's speed setting, without changing when they arrive). Lines are rotated sprites at their opacity, `LINE_LENGTH` (four field widths) by `LINE_THICKNESS` times their length and thickness, in their own color if they have one (red, green, and blue 0 to 1) or else the usual pale yellow, in order, so a later line covers an earlier one; notes are Phigros's colors (taps blue, drags yellow, flicks red, holds blue with a body), drawn from two field heights (and 10 s) out, and a hit bursts on the line.
- **Phigros's own charts** (`ImportPhigros`). Format 3, and format 1's moves packed as x × 1000 + y on an 880 by 520 screen. Times are in 1/32 beats at each line's BPM; `notesAbove` and `notesBelow` (type 1 tap, 2 drag, 3 hold, 4 flick) become notes on that side, move, rotate, and disappear events become keys, and speed events become speed keys. The chart's `offset` becomes `AudioOffsetUs`. `ReadPhigrosInfo` reads a package's info.yml (Phira's) or info.txt for the title, artist, charter, level, music, chart, and illustration. `songs::LoadMixedChartFile` loads a package from its info file.
- **Re:PhiEdit's charts** (`ImportRpe`; `ImportPhigros` hands them over when the JSON has `BPMList` or `META`). RPE is what most fan charts are made in, and what Phira plays. Times are beats as [bar, numerator, denominator] along one BPM list. Lines move on a 1350 by 900 canvas, centered, y up, turning clockwise (so rotation is negated), with alpha 0 to 255; a note's `positionX` is canvas units along its line (÷ 75.9375 for X units), `above` 1 or 2, types 1 tap, 2 hold, 3 flick, 4 drag. Speed is 120 canvas units a second per unit: ours is RPE's ÷ 4.5. Each line's events come in layers that add up; each event eases by one of RPE's 29 easings (or the part of one between `easingLeft` and `easingRight`). The import bakes them into keys: an eased event into straight pieces (16, or 40 for back, elastic, and bounce), with a second key a microsecond before each jump, and a speed ramp into 16 held steps of its average. A line with a `father` carries its parent's position, its own turned by the parent's rotation; its rotation is its own. META's `offset` (ms) becomes `AudioOffsetUs`. A line's extended events (one layer) scale and color it: Phira draws a plain line 6 of its units long (each half the canvas's width) times `scaleX`, so its length is `scaleX` × 3 field widths ÷ `LINE_LENGTH`; scaled across by `scaleY` (other than 1), it's 0.0076 units thick per unit of scale, else our usual thickness; `colorEvents` ease between [red, green, blue] 0 to 255. A storyboard draws with these, too: a plain line scaled 200 times thicker is a colored background, and short lines are marks. Lines are drawn in `zOrder` (lowest first), so the import sorts them and renumbers the notes' lines; a null line keeps its place, hidden. Fake notes are left out. A line with a `Texture` other than line.png is a picture (a storyboard): `JudgeLine::Picture` names the image, its `anchor` sits on the line's position, and a pixel of it is a canvas unit times the line's scale (`Length` and `Thickness` hold field units per pixel, so it stretches to the field as positions do and lines up with the lines: this chart's hit bar at its end is a column picture whose filled half starts at the line's height). `LinePlayfield::SetPicture` gives the playfield a picture's texture and size; lines draw their pictures, tinted by their color (white without one), instead of bars, and nothing until given one. `GetPictureUses` lists a chart's pictures with the largest each is drawn, so a game can load each no larger (the game loads them before a run, on workers, at most 1.5 pixels a field unit and 2048 a side, larger ones one at a time: enchanted love's 100 pictures, 830 MB of pixels decoded, load in 0.8 s on a desktop). Lines attached to the UI or showing text or paint hide unless they have notes, as those aren't drawn. Incline, GIFs, `bpmfactor`, and notes' `yOffset`, `size`, `alpha`, and `visibleTime` are left out. A package can be an archive: `songs::IsMapArchive` takes `.pez` (Phira's), and a file without an extension that is a zip (Phira's downloads), as well as `.osz`. On Android, files opened with the app or shared to it (the game's manifest lists the types: osu!'s archive type, zip, and plain data) are copied by `HyoshiActivity` into a folder of their own in the cache's `opened/`, under their own names, and arrive as dropped files (`Platform::TakeDroppedFiles`) once the native side is ready (`onNativeReady`, called at the end of `Platform::Initialize`); `ForgetDroppedFile` deletes the copies.
- **Not yet.** Text lines, paint, the UI lines are attached to, GIFs' animation (their first frame shows), the editor's preview of pictures, Phira's line color following the judgments (yellow until a Good, then blue, then white after a miss), notes' own `floorPosition` overrides, and PhiEdit's text format (PEC).

### 16.6 As built: Arcaea's lanes

Charts' `arcaea` notes are lane notes, read like Mania's, judged by `ArcaeaJudge` (`modes/mania`). Both lane judges derive from `ILaneJudge`, so `ManiaPlayfield` draws either, and Arcaea's are drawn on the track (`PlayfieldView::Track`).

- **Taps.** A press strikes the earliest tap in its lane within Far: Perfect (a max Pure) within 25 ms, Great (Pure) within 50, Good (Far) within 100. A tap not struck by then is missed (Lost).
- **Holds** aren't struck or released. A hold is a combo every half beat of its length (every beat from 255 BPM), at the tempo where it starts, each Perfect if its lane is held at that moment and missed if not. It can be pressed at any time, or touched (`ManiaAction::Touch`, a lane held without a press, which strikes no tap), and letting go only loses the combos until it's held again; a lane held before the hold starts has every combo. The first combo waits out the Far window, so it also takes a press up to 100 ms after the start. A hold finishes at its end, not its last combo (up to a combo's length before), so it's drawn to its end. `ArcaeaJudge::GetJudgmentCount` counts a chart's judgments for scoring.
- **Not yet.** Arcs, sky notes, and Arcaea's scoring.

---

## 17. Game Object Model, Scenes, and UI

### 17.1 Object model

Non-ECS, composition-based, deliberately simple:

- `Entity`: an ID (generational handle), a name, an optional parent, and a list of components
- `Component`: data plus behavior, owned by a type-specific manager
- **Managers update their component types in batches** in a fixed, explicit order, instead of every entity calling a virtual `Update()`
- Cross references use handles, never raw pointers, so destruction is always safe
- Inheritance depth: at most one level below `Component`

The object model is for things that are **few and unique**: UI widgets, background visuals, effects, scene flow. Notes are **never** entities.

### 17.2 Scenes

| Scene | Purpose |
|---|---|
| `BootScene` | Initialize subsystems, load global assets |
| `TitleScene` | Title screen |
| `SongSelectScene` | Browse songs, preview audio, pick difficulty and mode |
| `GameplayScene` | Owns the SongClock, active mode, judge, score, playfield |
| `ResultsScene` | Score, accuracy, judgment histogram, error graph |
| `CalibrationScene` | Audio and visual offset tests |
| `SettingsScene` | Offsets, scroll speed, volumes, input layout |

Scene transitions happen through a `SceneManager` with async loading (worker threads) and a transition screen.

As built (M4 menus, `game/scenes`): `SongSelectScene`, `GameplayScene` (Mania only for now), and `ResultsScene`; the settings are a panel over the song select rather than a scene, and there is no title, boot, or calibration scene yet. A scene updates and draws in one `RunFrame` call and asks for the next scene with a request (`PlayChart`, `ShowResults`, `GoToSongSelect`). `Application` switches between them, with a short fade, instead of a `SceneManager`. Loading happens inside the scenes, on workers: the song library scans on a worker, and gameplay shows "Loading" until its music and hit sounds are decoded. Scripted runs (`HYOSHI_CHART`, `HYOSHI_AUTOPLAY`, `HYOSHI_DEMO_BEATS`) start in gameplay. The song library (`game/SongLibrary`) reads osu!mania `.osu` files through the importer in memory, as well as `.rchart.json`, and groups a map set's difficulties with the same artist and title into one song.

As built (the engine split): `Scene` and the scene switching are the engine's (`app::Scene`, `app::Application`), and a scene moves on by handing the Application the next scene (`SwitchTo`) instead of a request the Application interprets, so the engine knows no game's scenes. A game implements `app::Game`: it sets itself up once the window, GPU, and audio exist (`Initialize`), gives the first scene (`CreateFirstScene`), and saves on backgrounding (`Save`). The launch screen below became the engine's splash (`SplashScene`), which says "Made with" above the logo and then shows the game's first scene. The song select, gameplay, and results scenes are the game's.

As built (M4, branding): the game now opens on `LaunchScene`, a boot screen that shows the logo for about two seconds (the stacked logo in portrait, the side-by-side one in landscape, with an accent line sweeping under it) and then goes to the song select. Any key or tap skips it. Scripted runs still skip straight to gameplay.

### 17.3 Game UI

- **v1:** a small custom immediate-mode UI drawn through `SpriteBatch` and `TextRenderer`, with touch-friendly widgets (button, slider, list, toggle). Enough for menus.
- **Later:** a retained layout system (anchors, safe area awareness, animation) when menus grow.
- **Dear ImGui is for debug and tools only**, never player-facing UI.

As built (M4 menus, `game/ui/Ui`): buttons, toggles, sliders with - and + steps, clickable areas, and scrolling (wheel, drag, flicks), with layout from rectangles cut off a larger one. Widgets draw as they're called; each draws its background on one layer and its text on another, since the sprite batch groups a layer's sprites by texture. Rounded corners are nine slices of a small circle distance field. A press and a release on the same widget, without a drag between, is a click; the first finger on a touchscreen acts as the pointer. Keys come as presses (with repeats) for scenes that navigate with the keyboard. `TextField` edits one line of text: a click or tap (or `FocusTextField`) gives it focus, typed text (UTF-8, from the platform's text input: `Platform::SetTextInput`, which also brings up a phone's on-screen keyboard, and `TakeTypedText`, passed in by the app each frame) adds to the end, Backspace takes off the last character, and Enter, Escape, or a click elsewhere ends it; those keys and letters go no further while it has focus, and `IsTyping` tells scenes to leave keys alone. A `TextFieldStyle` sets how a field looks and behaves: without its background or clear button, its text's size, color, weight, and alignment, its margin, and whether focusing it with a click selects all its text, so typing or Backspace replaces it (a value field's way). `IsFocused` and `ClearFocus` let a scene commit a field's value when it loses focus. The app turns text input on while a field has focus. Unit tests drive the UI headless with synthetic input. Dear ImGui now shows only with F1.

---

## 18. Assets and Content Pipeline

### 18.1 Asset identity

- Every asset has an `AssetId = xxHash64(normalized path)`.
- Game code references assets by ID (compile-time hashed where possible).
- An `AssetRegistry` maps IDs to cooked files and load state.

### 18.2 Cooking

The `asset-cooker` tool converts source content into runtime formats:

| Source | Cooked |
|---|---|
| PNG | ASTC (4x4 for UI and sprites, larger blocks where quality allows), packed into atlases |
| TTF/OTF | MSDF atlas + glyph metrics |
| `.rchart.json` | Binary `.rchart` |
| WAV/FLAC | Ogg Vorbis for music; short PCM or Ogg for hit sounds |
| `.slang` | SPIR-V (and later MSL) |

Fallback: if a device lacks ASTC support (rare on Android, common on Intel Macs), textures load as uncompressed RGBA8. Check `textureCompressionASTC_LDR` at startup.

### 18.3 Packaging

- Base install stays small: engine, UI assets, a few songs.
- Song packs are downloadable content, stored in the app's writable data directory.
- Android: assets inside the APK/AAB read through SDL's `SDL_IOStream`.

### 18.4 Hot reload (desktop)

File watcher on `content/` re-cooks and reloads textures, charts, and shaders while the game runs. **Later:** push charts to a connected phone over the network (adb forward or a small TCP server in debug builds) for on-device iteration.

---

## 19. Tools and Debugging

### 19.1 Debug overlay (ImGui, debug builds)

- **Clock panel:** raw vs smoothed song time, drift graph, audio callback interval histogram, estimated output latency
- **Input panel:** event timestamps vs processing time (shows input pipeline latency), active touches
- **Judgment panel:** live hit error graph, judgment histogram
- **Render panel:** frame time graph, draw calls, instance counts, GPU memory (from VMA)
- **Audio panel:** active voices, bus levels, route info

### 19.2 Profiling

- Tracy zones on every major system from M0
- Tracy frame marks per frame; audio callback instrumented separately
- Android profiling over the network with Tracy's client

### 19.3 GPU debugging

- **macOS (MoltenVK):** Vulkan validation layers; Xcode Metal frame capture works on MoltenVK output
- **Android:** Android GPU Inspector (AGI) for Vulkan frame capture and GPU counters; runs on macOS as the host
- RenderDoc is unavailable on macOS; use it only if a Windows or Linux machine enters the workflow

### 19.4 Chart editor (later, desktop)

Built on ImGui first:

- Waveform display, zoomable timeline, beat snapping (1/1 to 1/16, triplets)
- Timing point and SV editing
- Per-mode note placement tools
- Timeline track editing with easing previews
- Instant test play from any point

As built (editor framework, 2026-09-28): the chart editor will be a panel in an editor framework, [ADR 0002](decisions/0002-editor-framework.md). `engine/editor` (`hyoshi::editor`, desktop only) runs a game under its editor: the game full-window, with a menu bar and dockable ImGui panels over it (the docking branch of ImGui), and the dock space's empty center passes input to the game. A game implements `editor::Editor` and adds its own `editor::Panel`s, and `hyoshi_add_editor` builds the editor from the game's code as a library; the game's own executable never links the framework. `app::Application` takes the framework's side as an optional `app::EditorLayer`, and tells the game through `AppServices::IsEditor`. The ADR's milestones: the chart and timeline panel with play from a point in the chart next, then the game drawn in a panel (RHI render targets), then field descriptions, inspectors, and the object model of section 17.1.

---

## 20. Mobile Requirements Checklist

Every milestone from M1 onward must keep these true on the lowest-end test phone.

- [ ] Survives backgrounding and foregrounding mid-song without crashing or desyncing (returns to pause screen)
- [ ] Recreates surface and swapchain correctly after Android destroys the window
- [ ] Handles rotation and uses pre-rotation (no compositor rotation)
- [ ] Rendering capped to display refresh rate; menus idle at low frame rate
- [ ] Depth and MSAA attachments are transient; load/store actions are minimal
- [ ] No sustained thermal throttling in a 10 minute session (frame times stay stable)
- [ ] Audio resumes after interruptions (calls, other apps, notifications)
- [ ] Output route changes (Bluetooth connect/disconnect) detected; player prompted to recalibrate
- [ ] UI and playfield respect the safe area; layouts work from 4:3 to 21:9
- [ ] Memory stays within budget (Section 24); low memory events drop caches
- [ ] Touch timestamps come from the OS event time (verified)
- [ ] App size within budget; songs downloadable separately

---

## 21. Coding Standards

### 21.1 Naming

| Element | Style | Example |
|---|---|---|
| Functions and methods | PascalCase | `ThisIsAnyFunction()` |
| Private and protected member variables | camelCase | `smoothedTime` |
| Public member variables (including struct fields) | PascalCase | `HostTime` |
| Constants and `constexpr` values | SCREAMING_SNAKE_CASE | `THIS_IS_ANY_CONSTANT` |
| Classes, structs, enums, enum values | PascalCase | `SongClock`, `Judgment::Perfect` |
| Interfaces | `I` prefix + PascalCase | `IAudioBackend` |
| Local variables and parameters | camelCase (suggested) | `songTime` |
| Namespaces | lowercase | `hyoshi::audio` |
| Files | PascalCase matching the main type | `SongClock.h`, `SongClock.cpp` |

### 21.2 Braces

Allman style: every brace on its own line.

```cpp
void SongClock::Update(HostTimeNs now)
{
    if (isPaused)
    {
        return;
    }

    // ...
}
```

### 21.3 Enforcement

`.clang-format` (formatting):

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ColumnLimit: 120
BreakBeforeBraces: Allman
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
PointerAlignment: Left
NamespaceIndentation: None
SortIncludes: CaseSensitive
AccessModifierOffset: -4
BreakTemplateDeclarations: Yes
```

The last two options keep `public:`/`private:` flush with the class brace and put `template <...>` on its own line, matching the code samples in this document. Plain LLVM style would indent access specifiers by 2 and join short template declarations onto one line.

`.clang-tidy` (naming checks):

```yaml
Checks: '-*,readability-identifier-naming'
HeaderFilterRegex: '.*/(engine|modes|game|tools|tests)/.*'
CheckOptions:
  - { key: readability-identifier-naming.FunctionCase,          value: CamelCase }
  - { key: readability-identifier-naming.FunctionIgnoredRegexp, value: '^SDL_main$' }
  - { key: readability-identifier-naming.MethodCase,            value: CamelCase }
  - { key: readability-identifier-naming.ClassCase,             value: CamelCase }
  - { key: readability-identifier-naming.StructCase,            value: CamelCase }
  - { key: readability-identifier-naming.EnumCase,              value: CamelCase }
  - { key: readability-identifier-naming.EnumConstantCase,      value: CamelCase }
  - { key: readability-identifier-naming.PrivateMemberCase,     value: camelBack }
  - { key: readability-identifier-naming.ProtectedMemberCase,   value: camelBack }
  - { key: readability-identifier-naming.PublicMemberCase,      value: CamelCase }
  - { key: readability-identifier-naming.ConstexprVariableCase, value: UPPER_CASE }
  - { key: readability-identifier-naming.GlobalConstantCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.StaticConstantCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.LocalVariableCase,     value: camelBack }
  - { key: readability-identifier-naming.ParameterCase,         value: camelBack }
  - { key: readability-identifier-naming.NamespaceCase,         value: lower_case }
```

Note: in clang-tidy, `CamelCase` means PascalCase and `camelBack` means camelCase.

`-*` limits clang-tidy to the naming check (otherwise it also runs its default analyzer checks). `HeaderFilterRegex` makes it check the engine's own headers but not third-party ones. `SDL_main` is exempt because SDL renames `main` to it on Android and iOS.

Both run as build targets: `format`, `format-check`, and `tidy`.

### 21.4 Language rules

- C++20. No exceptions across module boundaries; use `Result<T>` for fallible operations. (Final policy on exceptions and RTTI: see Open Decisions.)
- No raw `new`/`delete` in engine code outside allocators; use `std::unique_ptr` or handles.
- `HYOSHI_ASSERT` for programmer errors (debug only), `HYOSHI_VERIFY` for checks kept in release.
- Headers include only what they need; prefer forward declarations.
- Real-time code (audio callback, judgment) is marked with a comment tag `// REALTIME` and reviewed for allocations and locks.

---

## 22. Build, CI, and Deployment

### 22.1 CMake presets

| Preset | Platform | Notes |
|---|---|---|
| `macos-debug` | macOS arm64 | Validation layers on, Tracy on, ImGui on |
| `macos-release` | macOS arm64 | |
| `android-arm64-debug` | Android arm64-v8a | Used by Gradle's `externalNativeBuild` |
| `android-arm64-release` | Android arm64-v8a | |

As built: `windows-debug` and `windows-release` build for Windows x64 with MSVC and Ninja, configured from a Visual Studio x64 developer environment. MSVC compiles everything with `/utf-8` (fmt requires it) and `/Zc:preprocessor` (for `__VA_OPT__`), and Hyoshi's targets with `/W4`. Executables embed `platforms/windows/Utf8.manifest`, which makes UTF-8 the process code page, so non-ASCII paths work through `argv`, environment variables, `std::filesystem`, and SDL. Windows is a development host, not a shipping target yet (Section 25, later milestones).

### 22.2 Android specifics

| Setting | Value | Reason |
|---|---|---|
| `minSdkVersion` | 26 | AAudio availability; Vulkan 1.1 devices |
| `targetSdkVersion` | Latest required by Google Play | Store requirement |
| ABI | `arm64-v8a` only (v1) | Nearly all Vulkan-capable devices |
| NDK | Latest LTS release | |
| Project base | SDL3's `android-project` template | Provides the Java activity glue |
| Manifest | `android.hardware.vulkan.version` feature declared; screen orientation set per game | Filters incompatible devices on Play |
| Debug builds | Bundle Vulkan validation layers | Driver issues surface early |

As built (the engine split): the Gradle logic below lives in the engine's `platforms/android/hyoshi-app.gradle`, which a game's `app/build.gradle` applies after setting its own namespace, application ID, versions, and CMake target. SDL's Java glue and `HyoshiActivity` come from the engine's `platforms/android/java`. The Gradle project itself (wrapper, manifest, icons, themes) is the game's.

As built (Android bring-up): `platforms/android` is a Gradle project (AGP 8.13, Gradle 8.14, NDK r27d, the SDK's CMake 4.1.2) whose `externalNativeBuild` runs the root `CMakeLists.txt` directly, with its own arguments rather than a CMake preset (Gradle picks the toolchain, ABI, and build type). The game builds as a shared library, `libmain.so`, with SDL linked in statically, so `HyoshiActivity` extends SDL's `SDLActivity` and loads only `main`. SDL's Java glue is copied from the pinned SDL release. ABIs: arm64-v8a and x86_64 (the emulator). The build copies the fonts and logos into a generated folder that Gradle packs as APK assets, and the game reads them by relative path through SDL, which reads assets on Android (`Platform::GetAssetPath`). The manifest requires Vulkan 1.1 and sets `fullUser` orientation; the game narrows it at run time from its Orientation setting. The window is fullscreen (immersive), the back button arrives as Escape, and on Android 12+ the splash screen shows the icon. Songs come from `songs/` in the app's external files folder (`Android/data/com.britoshi.hyoshi/files/songs`), which the player can fill over USB; the app's private data folder holds `settings.json`. Debug APKs also carry the maps in `content/charts` (a Gradle task wired to debug variants only, with an index of files and sizes, since assets can't be listed), and the game unpacks them into `dev-songs/` in its private data, copying only files that are missing or changed in size (the APK's folder is `bundled-songs/`: SDL reads a relative path from the app's files before its assets, so under one name it read back the old copies, and changed maps never updated), so a debug install has the same songs as the desktop build. Validation layers aren't bundled yet. Release builds are signed with the debug key until a release key exists. Checked on the API 36 x86_64 emulator (Vulkan through gfxstream on the host GPU, AAudio); not yet on a phone.

### 22.3 Shader build

`cmake/Shaders.cmake` runs `slangc` on every `.slang` file at build time, outputting SPIR-V into the build directory, with dependency tracking so edits rebuild automatically.

### 22.4 CI (GitHub Actions)

- macOS runner: build `macos-debug`, run unit tests and replay regression tests
- Android build job: build the debug APK (on a Linux or macOS runner)
- Lint job: clang-format check and clang-tidy naming check
- Later: upload APK artifacts for easy install on test phones

---

## 23. Testing Strategy

| Layer | Method |
|---|---|
| Core utilities, chart parsing, SV integration, timeline evaluation | doctest unit tests |
| SongClock | Unit tests with a fake audio backend that produces scripted clock snapshots (jitter, bursts, seeks) |
| Judgment | Deterministic tests: synthetic actions against small charts, asserting exact judgments |
| Full gameplay | **Replay regression tests**: recorded replays with expected final scores; must match exactly on every commit |
| Rendering | Manual visual checks per milestone; later, screenshot comparisons on desktop |
| Device | Manual checklist (Section 20) on every test phone per milestone |
| Latency | Measure end-to-end latency: tap-to-sound with a microphone recording, and tap-to-screen with a high frame rate phone camera recording |

---

## 24. Performance Budgets

Starting targets, to be revised with real measurements.

| Metric | Target |
|---|---|
| Frame rate | Locked to display refresh (60 minimum, 120 on capable devices) |
| Frame time at 120 Hz | 8.3 ms total, leaving thermal headroom |
| Gameplay CPU (clock, judgment, timeline, UI update) | Under 1 ms per frame |
| Draw calls in gameplay | Under 50 |
| Audio callback | Under 25 percent of its period on the lowest-end phone |
| Memory (low-end phone) | Under 400 MB total process memory during gameplay |
| Base install size | Under 150 MB (songs downloadable) |
| Song load time (select to playable) | Under 2 seconds |
| Input to judgment latency (engine side) | Under 1 frame from event arrival |

---

## 25. Roadmap and Milestones

Each milestone ends with a **"done when"** checklist. Do not start the next milestone until the previous one is done.

### M0: Foundation

- Repository layout (Section 6), CMake presets, CPM dependencies
- `.clang-format`, `.clang-tidy`
- Core: logging (spdlog, logcat on Android), asserts, `Result<T>`, handles
- SDL3 window on macOS and Android; lifecycle events logged
- Tracy integrated
- CI building macOS and Android

**Done when:** an empty window opens on the Mac and on an Android phone from the same codebase, logs appear in the terminal and in logcat, and Tracy connects to both.

### M1: Vulkan bring-up

- volk, VMA, instance and device creation (portability extensions on macOS)
- Swapchain with pre-rotation, recreation on resize, rotation, and background/foreground
- Render pass with clear, then a single triangle with a Slang shader
- Validation layers clean on both platforms
- Pipeline cache save and load
- **Verify SDL3 touch timestamp source on Android** (Section 12.3)

**Done when:** a triangle renders on Mac and Android, survives rotation and 20 background/foreground cycles without leaks or validation errors, and the touch timestamp question is answered.

### M2: 2D renderer

- RHI interface finalized for v1 (Section 10.1)
- `SpriteBatch` with instancing and texture atlases
- `Camera2D`, virtual resolution, safe area
- MSDF `TextRenderer`
- ImGui integrated for debug overlays
- Texture loading (RGBA8 first; ASTC through the cooker by the end of M2)

**Done when:** 10,000 animated sprites and on-screen text render at the display's refresh rate on the lowest-end phone, with the frame stats overlay visible.

### M3: Audio and the song clock

- `IAudioBackend` and the miniaudio backend (AAudio low latency on Android)
- Music decode on a worker thread, playback, pause, resume, seek
- Hit sound voice pool
- Audio clock snapshots, `SongClock` with smoothing, offsets
- Clock debug panel (raw vs smoothed, drift)
- Investigate AAudio timestamps for true output latency
- Route change detection

**Done when:** a song plays on Android with a metronome click and a visual beat flash that stay in sync for the full song, the clock never goes backwards, and seeking resyncs within one frame.

### M4: First playable (Mania on Android)

- Chart JSON loader, validation, SV scroll position precompute
- `NoteCursor`, Mania `IJudge`, scoring and combo
- GPU-positioned notes and long notes
- Touch lanes with finger tracking
- `GameplayScene` and a basic `ResultsScene`
- A handful of original or permissively licensed test charts

**Done when:** a full Mania chart is playable on the lowest-end Android phone with correct judgments, stable frame times, and it feels right to play with wired headphones.

### M5: Core hardening

- `CalibrationScene` (audio and visual offset tests), offsets saved per output route
- Replays: record, save, play back
- Replay regression tests in CI
- Unit tests for SongClock and judgment
- Pause menu with countdown and resync; interruption handling fully verified
- Latency measurements recorded (Section 23)

**Done when:** a replay recorded on the phone reproduces the exact same score on the Mac in CI, and calibration makes Bluetooth play feel correct.

### M6: Drum mode

- Mode registry and `IGameMode` interface finalized
- Drum mode: chart payload, input mapper, judge, playfield renderer

**Done when:** Drum mode works without changing any Rhythm Core code (only additions through interfaces). If core changes were needed, the interfaces are fixed first.

### M7: Timeline system and game UI

- `Track<T>`, easing library, target registry, chart timeline loading
- Immediate-mode game UI widgets; `SongSelectScene` with previews; `SettingsScene`
- Asset registry and async scene loading

**Done when:** a chart can animate the playfield (rotation, scale, opacity) in sync with music, and the game has a complete menu flow from title to results.

### M8: Circle mode

- 2D positional notes, approach rings, fade timing
- Slider curves (linear, Bezier, circular arc, Catmull-Rom), slider body mesh, follow judgment
- Direct touch and drag input

**Done when:** a Circle chart with sliders is fully playable on the phone with correct slider tracking.

### M9: Highway mode

- `Camera3D`, depth attachment (transient), perspective playfield
- Laser ribbons with analog judgment over time
- Virtual knobs via drag zones on touch; gamepad axes on desktop
- Camera timeline tracks (tilt, zoom)

**Done when:** a Highway chart with lasers and camera effects is playable on the phone at full frame rate.

### M10: Line mode

- Multiple animated judgment lines with per-line scroll integrals
- Notes attached to lines (tap, hold, drag, flick), above and below
- Gesture recognition (flick thresholds, drag tracking)
- Heavy timeline usage (dozens of tracks per chart)

**Done when:** a Line chart with moving, rotating lines and all four note kinds is playable on the phone with stable frame times.

### M11: Chart editor (desktop)

- ImGui-based editor per Section 19.4
- Mania first, then other modes
- Hot reload to device

**Done when:** a new chart can be authored from an audio file to a playable result without touching JSON by hand.

### Later milestones (unordered)

- **Metal backend and iOS** (once an iOS test device is available)
- Android Frame Pacing (Swappy)
- Streaming music decode
- Scripting (Lua) for game logic
- Retained UI layout system
- Community format importers (`.osu`, `.sm`)
- Windows and Linux builds (as built: Windows builds for development, Section 22.1)
- FMOD or Wwise backend as a proof of the audio abstraction

---

## 26. Getting Started: Week One

A concrete sequence for the first sessions, covering M0.

1. **Create the repo** with the layout from Section 6 (empty folders with `.gitkeep` where needed). Commit this design doc to `docs/DESIGN.md`.
2. **Install tooling on the Mac:** Xcode command line tools, CMake, Ninja, the Vulkan SDK (includes MoltenVK and validation layers), Android Studio with the NDK and CMake packages, and `slangc`.
3. **Root `CMakeLists.txt`:** set C++20, add CPM, pull SDL3, spdlog, and Tracy first. Add the `engine` library target and a `game` executable target.
4. **Add `.clang-format` and `.clang-tidy`** from Section 21 and run them once on the starter files.
5. **Core basics:** `Log.h` (spdlog wrapper, logcat sink on Android), `Assert.h`, `Result.h`, `Handle.h`.
6. **Platform:** `Platform::Initialize`, window creation, event pump, lifecycle callbacks that just log for now.
7. **Mac run:** window opens, events print.
8. **Android:** copy SDL3's `android-project` into `platforms/android/`, point its Gradle `externalNativeBuild` at the root CMake with the `android-arm64-debug` preset, set `minSdkVersion 26` and `arm64-v8a`.
9. **Android run:** install on the phone, confirm the window, logcat output, and lifecycle logs when switching apps.
10. **Tracy:** connect the Tracy profiler to both the Mac build and the phone.
11. **CI:** a GitHub Actions workflow building the macOS preset and the Android APK.
12. **Write ADR 0001** in `docs/decisions/` recording the stack choices. Then M0 is done.

---

## 27. Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Android audio latency varies widely by device | Timing feels off on some phones | AAudio low latency mode, AAudio timestamps, per-route calibration, test on multiple phones |
| SDL3 touch timestamps not from OS event time | Judgment inaccuracy on touch | Verify in M1; native hook fallback |
| Android Vulkan driver bugs | Crashes or artifacts on some devices | Vulkan 1.1 baseline, conservative features, validation in debug, testing on cheap devices |
| MoltenVK behavior differs from native Vulkan | Bugs hidden on Mac, surfacing on Android | Android is the source of truth; test on device every milestone |
| Bluetooth latency | Poor experience for a large share of players | Calibration flow, route detection, clear UX prompts |
| Scope: five modes is a lot for one developer | Burnout, never shipping | Strict milestone gates; M4 is a real playable; each mode is optional after the core is proven |
| Time constraints alongside school and other work | Slow progress | Small milestones with visible results; the "done when" lists keep scope honest |
| IP issues with reference styles | Legal trouble when shipping | Reference styles are internal only; shipped games use original identity and content |
| Over-engineering the RHI before it renders anything | Stalled progress | RHI grows from real needs; M1 renders a triangle before the interface is formalized in M2 |

---

## 28. Open Decisions

| Topic | Options | Decide by |
|---|---|---|
| Exceptions and RTTI | Disable both engine-wide vs allow in tools only | M0 |
| Chart file extensions | `.rchart.json` / `.rchart` placeholders | M4 |
| Default hit windows and scoring formula | Tune by feel during M4 and M5 | M5 |
| Orientation for the first game | Portrait vs landscape vs per-mode (decided: both, adaptive; decision 17) | M4 |
| Game UI approach long term | Custom retained system vs a library | M7 |
| Scripting language | Lua (likely) vs none vs C# | After M7 |
| Engine license | Proprietary vs open source (and which license) | Before any public release |
| Metal timing | When an iOS device is available | Later |

---

## 29. Decision Log

| # | Decision | Reason |
|---|---|---|
| 1 | C++20 with Vulkan as the primary graphics API | Native on Android and desktop; runs on Apple via MoltenVK |
| 2 | Build a solid Vulkan foundation and a 2D renderer first, not a full 3D renderer | 2D is 3D with an orthographic camera; rhythm games need batching and text far more than 3D features |
| 3 | Perspective and depth support planned into the renderer early | The Highway mode requires it |
| 4 | miniaudio as the default audio backend, behind `IAudioBackend` | Free, permissive, full control over the timing path; FMOD or Wwise can be added as backends for studios |
| 5 | Audio clock is the master clock | Frame timing is too imprecise for judgment |
| 6 | Non-ECS, composition-based object model; data-oriented note processing | Easier tooling and authoring; rhythm games do not need ECS scale; hot paths still use packed arrays |
| 7 | Note processing and judgment stay single threaded; note visuals computed on the GPU | Workload is tiny; judgment must be ordered and deterministic |
| 8 | Engine core knows about timed events, not lanes; modes are plugins | Circle and Line styles have no fixed lanes |
| 9 | Mobile-forward, Android as the primary device | Target audience; available test hardware |
| 10 | Vulkan 1.1 baseline with traditional render passes (not dynamic rendering) | Broad Android device support; maps cleanly to tile-based GPUs and to Metal |
| 11 | Metal backend deferred until an iOS test device is available | Simulator cannot validate latency, touch, or performance |
| 12 | SDL3 for platform | Covers Android, iOS, macOS, Windows, Linux with high resolution event timestamps |
| 13 | Integer microseconds for all gameplay timing | Determinism across devices; reproducible replays |
| 14 | Reference games are validation targets only | Shipped games need their own identity and content |
| 15 | Engine named Hyoshi; C++ namespace `hyoshi`, macro prefix `HYOSHI_` | Closes the M0 naming decision |
| 16 | Song time follows the music cursor through one filtered host-to-song mapping, used for both visuals and judgment (Section 9, "As built") | Raw snapshot extrapolation jitters with callback scheduling; the filtered mapping converges on the true offset, and a cursor-based clock survives pause, seek, and device restarts |
| 17 | Menus and gameplay adapt to either orientation: the canvas's short side is fixed at 1080, and each screen has a landscape and a portrait layout (Section 10.4, "As built") | Phones get held both ways, and the desktop window can be either shape; one canvas rule keeps every layout in the same units |
| 18 | Text uses single-channel distance fields generated at runtime from font outlines, for now, rather than MSDF cooked offline (Section 10.3, "As built (text)") | No offline font pipeline yet, and runtime glyphs cover any song title, Japanese included, without baking a character set; quality is enough for UI sizes |
| 19 | Music files stream, decoded about 1.4 s ahead on a thread per song; sounds stay fully decoded (Section 11.2, "As built (streaming music)") | Decoding a whole song took hundreds of milliseconds before it could play, and tens of megabytes; streaming starts in a few milliseconds with 256 KB of buffers |

---

## 30. Glossary

| Term | Meaning |
|---|---|
| **AAudio** | Android's low latency native audio API |
| **ADR** | Architecture Decision Record: a short document recording one decision and its reasoning |
| **ASTC** | Adaptive Scalable Texture Compression, the standard mobile texture format |
| **DSP clock** | Count of audio frames consumed by the audio device |
| **Host clock** | The engine's monotonic system clock (`SDL_GetTicksNS`) |
| **Judgment** | Classification of a hit by timing error (Perfect, Great, Good, Bad, Miss) |
| **MoltenVK** | A Vulkan implementation layered on Apple's Metal |
| **MSDF** | Multi-channel Signed Distance Field, used for sharp scalable text |
| **Pre-rotation** | Rendering in the display's native orientation so the Android compositor does not rotate each frame |
| **RHI** | Rendering Hardware Interface: the engine's backend-agnostic graphics layer |
| **Scroll position** | Precomputed integral of scroll velocity up to a note's time, used for GPU note placement |
| **Song time** | Current playback position of the chart's audio, corrected for latency and offsets |
| **SV** | Scroll velocity: chart-controlled changes to note scrolling speed |
| **Tile-based GPU** | Mobile GPU architecture that renders the screen in small tiles in on-chip memory |
| **Transient attachment** | A render target that only needs to exist in tile memory during a render pass |
| **VMA** | Vulkan Memory Allocator library |
