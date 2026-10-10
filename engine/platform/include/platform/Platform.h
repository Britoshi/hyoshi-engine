#pragma once

#include "core/Result.h"
#include "core/Time.h"
#include "input/InputQueue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace hyoshi::platform
{

// The host clock (SDL_GetTicksNS). Thread-safe, and usable as a HostClockFn.
HostTimeNs ReadHostClock();

// Reads a whole file. On Android, relative paths resolve inside the APK's assets. Thread-safe, so
// worker jobs can use it.
Result<std::vector<std::byte>> LoadFile(const std::string& path);

// Writes a whole file. The old file is replaced only once the new one is complete, so a crash
// midway leaves it intact. For the user data directory; APK assets are read-only.
Result<void> SaveFile(const std::string& path, std::span<const std::byte> contents);

// Creates a directory and any missing parents. Succeeds if it already exists.
Result<void> MakeDirectory(const std::string& path);

enum class LifecycleEvent
{
    WillEnterBackground,
    DidEnterBackground,
    WillEnterForeground,
    DidEnterForeground,
    LowMemory,
    Terminating
};

const char* ToString(LifecycleEvent event);

struct PlatformConfig
{
    std::string AppName = "Hyoshi";
    std::string WindowTitle = "Hyoshi";
    int32_t WindowWidth = 1280;
    int32_t WindowHeight = 720;

    // Loads the Vulkan library and creates the window with Vulkan support.
    bool EnableVulkan = false;
};

struct PixelSize
{
    int32_t Width = 0;
    int32_t Height = 0;
};

// A rectangle in window coordinates normalized to 0..1, like touch and pointer input.
struct NormalizedRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 1.0f;
    float Height = 1.0f;
};

enum class Orientation : uint8_t
{
    // Follow the device (phones) or leave the window as it is (desktops).
    Auto,
    Landscape,
    Portrait
};

// Receives app lifecycle events as soon as the OS reports them. It can run on a thread other than
// the main thread (on Android, the Java UI thread), so it must be quick and thread-safe.
using LifecycleCallback = std::function<void(LifecycleEvent)>;

// Sees every raw SDL event before the platform handles it. For the debug UI integration, which
// DESIGN.md section 7 allows to use SDL directly.
using EventHook = std::function<void(const SDL_Event&)>;

struct LifecycleState;

// Wraps SDL3: window, event pump, lifecycle, host clock. No other module includes SDL headers.
class Platform
{
public:
    Platform();
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    // Set before Initialize so that no lifecycle event is missed.
    void SetLifecycleCallback(LifecycleCallback callback);

    Result<void> Initialize(const PlatformConfig& config);
    void Shutdown();

    void SetEventHook(EventHook hook);

    // Drains all pending OS events: handles window and lifecycle events, and turns keyboard,
    // touch, and pointer events into InputEvents on the input queue.
    void PumpEvents();

    // This frame's input, for the game to take after PumpEvents.
    input::InputQueue& GetInputQueue()
    {
        return inputQueue;
    }

    // Blocks until an event arrives or the timeout elapses. A negative timeout waits indefinitely.
    void WaitForEvents(int32_t timeoutMs = -1);

    // True once after the window's size in pixels changes.
    bool ConsumeWindowResized();

    // Files dropped on the window, or chosen with BrowseForFiles, since the last call. On the web,
    // the browser's files are copied into memory first (SDL puts them under /tmp/filedrop/).
    std::vector<std::string> TakeDroppedFiles();

    // Done with a file from TakeDroppedFiles: deletes the platform's copy of it, if it made one (the
    // web's, in memory). The player's own files, dropped on a desktop window, are left alone.
    void ForgetDroppedFile(const std::string& path);

    // Opens the browser's file picker, showing files that match accept (".osz"); the ones the
    // player chooses arrive with TakeDroppedFiles, as if dropped on the window. Call it from a
    // click or key press, since browsers open pickers only for those. Web only for now: elsewhere it
    // returns false (SDL_ShowOpenFileDialog could do it on desktops).
    bool BrowseForFiles(const std::string& accept);

    bool IsQuitRequested() const;

    // Ends the main loop as closing the window does.
    void RequestQuit();

    // True between entering the background and returning to the foreground.
    bool IsSuspended() const;

    HostTimeNs GetHostTimeNs() const;

    // Waits until the host clock reaches time, to well under a millisecond (it spins at the end).
    void SleepUntil(HostTimeNs time) const;

    // The refresh rate of the window's display in Hz, or 0 when unknown.
    float GetRefreshRate() const;

    // Size of the window's drawable area in pixels (2x the logical size on Retina displays).
    PixelSize GetWindowPixelSize() const;

    // Sets the window's logical size (desktop only; mobile windows are fullscreen).
    void SetWindowSize(int32_t width, int32_t height);

    // Desktops: fill the window's display without a border, at the desktop's own resolution, or go
    // back to a window. Phones are always fullscreen. On a Mac the window moves to its own Space,
    // which takes a moment: IsFullscreen catches up once it has.
    void SetFullscreen(bool isFullscreen);

    // Whether the window fills its display, however it got there (SetFullscreen, or a Mac window's
    // own button and menu).
    bool IsFullscreen() const;

    // Hides the pointer and keeps it in the window while the window has focus, for a game that draws
    // its own and reads the mouse raw, or aims with something else (a trackpad's positions on the
    // pad). Pointer events still arrive: their position moves by the mouse's own motion, without
    // the system's pointer speed or acceleration (SetPointerSpeed scales it), and stays in the
    // window. Desktop only; unlocking puts the system's pointer there and shows it again, unless
    // SetCursorHidden hid it.
    void SetPointerLocked(bool isLocked);
    bool IsPointerLocked() const;

    // While the pointer is locked, how far it moves for each count of the mouse's own motion, in
    // window units (1, the default, is one each).
    void SetPointerSpeed(float speed);

    // Hides the system's mouse cursor, for a game that draws its own (on the web too), from the
    // next ApplyCursor.
    void SetCursorHidden(bool isHidden)
    {
        isCursorHidden = isHidden;
    }

    // Shows or hides the system cursor as SetCursorHidden asked. The Application calls it every
    // frame, except while the debug UI's windows show: they need the system's cursor.
    void ApplyCursor();

    // The mouse is over the window, so a cursor drawn by the game belongs there.
    bool IsPointerOverWindow() const;

    // Writable per-user directory for settings and caches, ending in a path separator.
    const std::string& GetUserDataPath() const;

    // The directory holding the executable and the files installed next to it (fonts), ending in
    // a path separator. Empty if unknown.
    const std::string& GetBasePath() const;

    // A file shipped with the game, like "fonts/x.otf": next to the executable on desktops, in the
    // APK's assets on Android.
    std::string GetAssetPath(const std::string& relative) const;

    // Where the player can put files for the game (songs), ending in a path separator: on Android,
    // the app's folder in shared storage (Android/data/<package>/files/), reachable over USB. Empty
    // elsewhere.
    std::string GetSharedStoragePath() const;

    // The window's icon on desktops: RGBA8 pixels, rows top to bottom.
    void SetWindowIcon(const uint8_t* rgba, int32_t width, int32_t height);

    // The part of the window clear of notches, rounded corners, and system bars.
    NormalizedRect GetSafeArea() const;

    // Phones: lock the screen to landscape or portrait (either way up), or follow the sensor.
    // Desktops: reshape the window to match, within the display.
    void SetOrientation(Orientation orientation);

    // For the RHI's surface creation, which DESIGN.md section 7 allows to use SDL directly.
    SDL_Window* GetNativeWindow() const;

private:
    Result<void> LoadVulkanLibrary();

    std::unique_ptr<LifecycleState> lifecycle;
    EventHook eventHook;
    input::InputQueue inputQueue;
    SDL_Window* window = nullptr;
    std::string userDataPath;
    std::string basePath;
    bool isInitialized = false;
    bool isVulkanLoaded = false;
    bool isWindowResized = false;
    bool isCursorHidden = false;
    std::vector<std::string> droppedFiles;
};

} // namespace hyoshi::platform
