# References

External documentation, libraries, and write-ups behind the decisions in [DESIGN.md](DESIGN.md).
Every link was checked on 2026-09-24.

## Pinned dependency versions

The version of each library the build pins (the latest release as of 2026-09-24).

| Library | Version | Used from | Link |
|---|---|---|---|
| SDL3 | `release-3.4.16` | M0 | https://github.com/libsdl-org/SDL |
| spdlog | `v1.17.0` | M0 | https://github.com/gabime/spdlog |
| doctest | `v2.5.3` | M0 | https://github.com/doctest/doctest |
| CPM.cmake | `v0.43.2` | M0 | https://github.com/cpm-cmake/CPM.cmake |
| Tracy | `v0.14.1` | M0 (not wired yet) | https://github.com/wolfpld/tracy |
| Vulkan-Headers | `vulkan-sdk-1.4.357.0` | M1 | https://github.com/KhronosGroup/Vulkan-Headers |
| volk | `1.4.350` | M1 | https://github.com/zeux/volk |
| Vulkan Memory Allocator | `v3.4.0` | M1 | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| stb | commit `2c980bb` (2026-08-02) | M1 (`stb_image_write` for screenshots; `stb_image` for logos since M4) | https://github.com/nothings/stb |
| Slang (`slangc`, prebuilt, SHA-256 checked) | `v2026.18.2` | M1 | https://github.com/shader-slang/slang |
| glm | `1.0.3` | M2 | https://github.com/g-truc/glm |
| Dear ImGui | `v1.92.9b` | M2 | https://github.com/ocornut/imgui |
| miniaudio (with stb_vorbis from the pinned stb) | `0.11.25` | M3 | https://github.com/mackron/miniaudio |
| yyjson | `0.13.0` | M4 | https://github.com/ibireme/yyjson |
| Noto Sans JP (static OTF subsets, Regular and Bold; SIL Open Font License 1.1) | noto-cjk `Sans2.004` | M4 (game UI) | https://github.com/notofonts/noto-cjk |

On the macOS development host, the Vulkan runtime comes from Homebrew: vulkan-loader 1.4.357, MoltenVK 1.4.2, and vulkan-validationlayers 1.4.357.

The Android build pins its tools too. The engine's `platforms/android/hyoshi-app.gradle` pins what builds the native code; a game's Gradle project pins Gradle and the plugin (the versions the first game uses):

| Tool | Version | Where it's pinned |
|---|---|---|
| NDK (LTS) | `27.3.13750724` (r27d) | `hyoshi-app.gradle`, `ndkVersion` |
| CMake (the SDK's package) | `4.1.2` | `hyoshi-app.gradle`, `externalNativeBuild.cmake.version` |
| compileSdk, targetSdk / minSdk | 36 / 26 | `hyoshi-app.gradle` |
| SDL's Java glue (`org.libsdl.app`) | from SDL `release-3.4.16` | copied into `platforms/android/java`; must match the SDL3 pin |
| Gradle (wrapper, SHA-256 checked) | `8.14.5` | the game's `android/gradle/wrapper/gradle-wrapper.properties` |
| Android Gradle Plugin | `8.13.2` | the game's `android/build.gradle` |

Licenses must be re-checked at the pinned version before shipping (DESIGN.md §4).

## Platform: SDL3

- [SDL3 wiki](https://wiki.libsdl.org/SDL3/FrontPage): API reference
- [README-android](https://wiki.libsdl.org/SDL3/README-android): Gradle project, activity glue, lifecycle behavior on Android
- [`android-project` template](https://github.com/libsdl-org/SDL/tree/main/android-project): the base for `platforms/android/` (the Java glue) and a game's Gradle project
- [README-main-functions](https://wiki.libsdl.org/SDL3/README-main-functions): `SDL_main` and the optional main-callbacks model
- [`SDL_EventType`](https://wiki.libsdl.org/SDL3/SDL_EventType): lifecycle events and their handling requirements
- [`SDL_AddEventWatch`](https://wiki.libsdl.org/SDL3/SDL_AddEventWatch): where mobile lifecycle events must be handled
- [`SDL_GetTicksNS`](https://wiki.libsdl.org/SDL3/SDL_GetTicksNS): the host clock (DESIGN.md §9)
- [`SDL_Vulkan_CreateSurface`](https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface)
- [`SDL_GetWindowSafeArea`](https://wiki.libsdl.org/SDL3/SDL_GetWindowSafeArea)
- [`SDL_IOFromFile`](https://wiki.libsdl.org/SDL3/SDL_IOFromFile): reads APK assets on Android

## Graphics: Vulkan

### Specification and guides

- [Vulkan specification](https://docs.vulkan.org/spec/latest/index.html)
- [Vulkan Guide (Khronos)](https://docs.vulkan.org/guide/latest/index.html)
- [Khronos Vulkan Tutorial](https://docs.vulkan.org/tutorial/latest/00_Introduction.html) and the original [vulkan-tutorial.com](https://vulkan-tutorial.com/)
- [vkguide.dev](https://vkguide.dev/): engine-oriented Vulkan guide
- [Khronos Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples): includes the performance samples for mobile and tile-based GPUs

### Android

- [Vulkan on Android (NDK graphics)](https://developer.android.com/ndk/guides/graphics)
- [Vulkan pre-rotation](https://developer.android.com/games/optimize/vulkan-prerotation): DESIGN.md §10.2
- [Android Vulkan Profiles (formerly Android Baseline Profile)](https://developer.android.com/ndk/guides/graphics/android-baseline-profile)
- [Android Frame Pacing (Swappy)](https://developer.android.com/games/sdk/frame-pacing)
- [Android GPU Inspector](https://developer.android.com/agi)

### macOS

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home): MoltenVK, loader, validation layers

### Libraries

- [volk](https://github.com/zeux/volk): meta-loader
- [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) and its [documentation](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/)
- [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap)
- [Slang](https://github.com/shader-slang/slang) and the [Slang user guide](https://shader-slang.org/slang/user-guide/)

## Audio

- [miniaudio](https://miniaud.io/): [programming manual](https://miniaud.io/docs/manual/index.html), [source](https://github.com/mackron/miniaudio)
- [dr_libs](https://github.com/mackron/dr_libs): dr_wav, dr_flac
- [stb](https://github.com/nothings/stb): stb_vorbis, stb_image

### Android audio

- [AAudio guide](https://developer.android.com/ndk/guides/audio/aaudio/aaudio)
- [Audio latency on Android](https://developer.android.com/ndk/guides/audio/audio-latency)
- [NDK audio reference](https://developer.android.com/ndk/reference/group/audio): includes `AAudioStream_getTimestamp` (DESIGN.md §11.4)
- [Oboe](https://github.com/google/oboe) (Apache-2.0) and its [full guide](https://github.com/google/oboe/blob/main/docs/FullGuide.md): Google's AAudio wrapper; worth reading for device workarounds even if unused
- [Native Audio (Exceed7)](https://exceed7.com/native-audio/) and the author's blog [Game Torrahod](https://gametorrahod.com/): in-depth mobile audio latency write-ups from a rhythm game developer (Unity-focused, but the platform findings apply)

## Input timestamps

- [Android `MotionEvent`](https://developer.android.com/reference/android/view/MotionEvent): event time and historical samples (DESIGN.md §12.3)
- [`View.requestUnbufferedDispatch`](https://developer.android.com/reference/android/view/View#requestUnbufferedDispatch(int)): opts out of vsync-batched input delivery
- [`UITouch.timestamp`](https://developer.apple.com/documentation/uikit/uitouch/timestamp): the iOS equivalent (later)

## Timing and determinism

- [Coding to the Beat: Under the Hood of a Rhythm Game in Unity](https://www.gamedeveloper.com/audio/coding-to-the-beat---under-the-hood-of-a-rhythm-game-in-unity): the DSP clock versus frame time
- [Floating Point Determinism (Gaffer On Games)](https://gafferongames.com/post/floating_point_determinism/)
- [Floating-Point Determinism (Bruce Dawson)](https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/): compiler, FMA, and libm sources of cross-platform divergence; relevant to replays (DESIGN.md §14.5)

## Reference games and formats

Validation targets only (DESIGN.md §1). Read the code for ideas; check the license before copying anything.

| Project | License | Why it's useful |
|---|---|---|
| [osu! (lazer)](https://github.com/ppy/osu) | MIT | Clock handling, judgment, slider curves, replays |
| [osu-framework](https://github.com/ppy/osu-framework) | MIT | Audio clock and frame timing underneath osu! |
| [StepMania](https://github.com/stepmania/stepmania) | see repo | `.sm`/`.ssc` formats, beat-based timing |
| [Etterna](https://github.com/etternagame/etterna) | MIT | StepMania fork focused on judgment and replays |
| [Phira](https://github.com/TeamFlos/phira) | GPL-3.0 | Open Line-style player. **GPL: read only, never copy code** |

- [osu! file format (`.osu`)](https://osu.ppy.sh/wiki/en/Client/File_formats/osu_%28file_format%29): for the planned importer (DESIGN.md §13.4)
- [osu! Overall Difficulty](https://osu.ppy.sh/wiki/en/Beatmap/Overall_difficulty): how hit windows scale with difficulty

## Tools and other libraries

- [Tracy](https://github.com/wolfpld/tracy)
- [spdlog](https://github.com/gabime/spdlog)
- [doctest](https://github.com/doctest/doctest)
- [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [GLM](https://github.com/g-truc/glm)
- [yyjson](https://github.com/ibireme/yyjson)
- [xxHash](https://github.com/Cyan4973/xxHash)
- [msdf-atlas-gen](https://github.com/Chlumsky/msdf-atlas-gen)
- [astc-encoder](https://github.com/ARM-software/astc-encoder)
- [clang-format style options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html)
- [clang-tidy `readability-identifier-naming`](https://clang.llvm.org/extra/clang-tidy/checks/readability/identifier-naming.html)
