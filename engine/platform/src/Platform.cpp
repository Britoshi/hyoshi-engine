#include "platform/Platform.h"

#include "core/Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>

namespace hyoshi::platform
{

// Shared with the SDL event watch, which may run on another thread.
struct LifecycleState
{
    LifecycleCallback Callback;
    std::atomic<bool> IsSuspended = false;
    std::atomic<bool> IsQuitRequested = false;
};

namespace
{

bool ToLifecycleEvent(Uint32 type, LifecycleEvent& outEvent)
{
    switch (type)
    {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        outEvent = LifecycleEvent::WillEnterBackground;
        return true;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        outEvent = LifecycleEvent::DidEnterBackground;
        return true;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        outEvent = LifecycleEvent::WillEnterForeground;
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        outEvent = LifecycleEvent::DidEnterForeground;
        return true;
    case SDL_EVENT_LOW_MEMORY:
        outEvent = LifecycleEvent::LowMemory;
        return true;
    case SDL_EVENT_TERMINATING:
        outEvent = LifecycleEvent::Terminating;
        return true;
    default:
        return false;
    }
}

// SDL requires mobile lifecycle events to be handled in an event watch: by the time the main loop
// pumps them, the OS may already have suspended the app.
bool SDLCALL OnEventWatch(void* userData, SDL_Event* event)
{
    LifecycleEvent lifecycleEvent{};
    if (!ToLifecycleEvent(event->type, lifecycleEvent))
    {
        return true;
    }

    auto* state = static_cast<LifecycleState*>(userData);
    switch (lifecycleEvent)
    {
    case LifecycleEvent::DidEnterBackground:
        state->IsSuspended = true;
        break;
    case LifecycleEvent::WillEnterForeground:
        state->IsSuspended = false;
        break;
    case LifecycleEvent::Terminating:
        state->IsQuitRequested = true;
        break;
    default:
        break;
    }

    if (state->Callback)
    {
        state->Callback(lifecycleEvent);
    }

    return true;
}

const char* WindowEventName(Uint32 type)
{
    switch (type)
    {
    case SDL_EVENT_WINDOW_SHOWN:
        return "shown";
    case SDL_EVENT_WINDOW_HIDDEN:
        return "hidden";
    case SDL_EVENT_WINDOW_EXPOSED:
        return "exposed";
    case SDL_EVENT_WINDOW_MINIMIZED:
        return "minimized";
    case SDL_EVENT_WINDOW_MAXIMIZED:
        return "maximized";
    case SDL_EVENT_WINDOW_RESTORED:
        return "restored";
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "focus gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "focus lost";
    case SDL_EVENT_WINDOW_OCCLUDED:
        return "occluded";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        return "display changed";
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
        return "safe area changed";
    default:
        return nullptr;
    }
}

// How long the event waited between its timestamp and the pump, for checking timestamp sources.
double PumpDelayMs(const SDL_Event& event)
{
    return static_cast<double>(SDL_GetTicksNS() - event.common.timestamp) / static_cast<double>(NS_PER_MS);
}

double TimestampMs(const SDL_Event& event)
{
    return static_cast<double>(event.common.timestamp) / static_cast<double>(NS_PER_MS);
}

} // namespace

const char* ToString(LifecycleEvent event)
{
    switch (event)
    {
    case LifecycleEvent::WillEnterBackground:
        return "WillEnterBackground";
    case LifecycleEvent::DidEnterBackground:
        return "DidEnterBackground";
    case LifecycleEvent::WillEnterForeground:
        return "WillEnterForeground";
    case LifecycleEvent::DidEnterForeground:
        return "DidEnterForeground";
    case LifecycleEvent::LowMemory:
        return "LowMemory";
    case LifecycleEvent::Terminating:
        return "Terminating";
    }
    return "Unknown";
}

Platform::Platform() : lifecycle(std::make_unique<LifecycleState>())
{
}

Platform::~Platform()
{
    Shutdown();
}

void Platform::SetLifecycleCallback(LifecycleCallback callback)
{
    HYOSHI_ASSERT(!isInitialized, "Set the lifecycle callback before Initialize");
    lifecycle->Callback = std::move(callback);
}

Result<void> Platform::Initialize(const PlatformConfig& config)
{
    HYOSHI_ASSERT(!isInitialized);

    // Android's back button arrives as a key (Escape, below) instead of closing the activity.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return Error{std::string("SDL_Init failed: ") + SDL_GetError()};
    }

    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_ANDROID)
    // Immersive: the status and navigation bars hide until swiped in. Layouts keep to the safe
    // area, clear of cutouts.
    windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif
    if (config.EnableVulkan)
    {
        if (Result<void> result = LoadVulkanLibrary(); !result)
        {
            SDL_Quit();
            return result;
        }
        windowFlags |= SDL_WINDOW_VULKAN;
    }

    if (char* prefPath = SDL_GetPrefPath(nullptr, config.AppName.c_str()))
    {
        userDataPath = prefPath;
        SDL_free(prefPath);
    }
    else
    {
        HYOSHI_LOG_WARN("No user data directory: {}", SDL_GetError());
    }
    if (const char* base = SDL_GetBasePath())
    {
        basePath = base;
    }

    if (!SDL_AddEventWatch(OnEventWatch, lifecycle.get()))
    {
        std::string message = std::string("SDL_AddEventWatch failed: ") + SDL_GetError();
        SDL_Quit();
        return Error{std::move(message)};
    }

    window = SDL_CreateWindow(config.WindowTitle.c_str(), config.WindowWidth, config.WindowHeight, windowFlags);
    if (window == nullptr)
    {
        std::string message = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_RemoveEventWatch(OnEventWatch, lifecycle.get());
        SDL_Quit();
        return Error{std::move(message)};
    }

    isInitialized = true;

    const int version = SDL_GetVersion();
    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    HYOSHI_LOG_INFO("SDL {}.{}.{}, video driver '{}', window {}x{} ({}x{} pixels)", SDL_VERSIONNUM_MAJOR(version),
                    SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version), SDL_GetCurrentVideoDriver(),
                    config.WindowWidth, config.WindowHeight, pixelWidth, pixelHeight);

    return {};
}

void Platform::Shutdown()
{
    if (!isInitialized)
    {
        return;
    }

    SDL_DestroyWindow(window);
    window = nullptr;
    if (isVulkanLoaded)
    {
        SDL_Vulkan_UnloadLibrary();
        isVulkanLoaded = false;
    }
    SDL_RemoveEventWatch(OnEventWatch, lifecycle.get());
    SDL_Quit();
    isInitialized = false;
}

Result<void> Platform::LoadVulkanLibrary()
{
    if (SDL_Vulkan_LoadLibrary(nullptr))
    {
        isVulkanLoaded = true;
        return {};
    }

    std::string message = std::string("Could not load the Vulkan library: ") + SDL_GetError();

#if defined(HYOSHI_VULKAN_LOADER_PATH)
    // Development fallback: a loader outside the default search path, found at configure time
    // (Homebrew's /opt/homebrew/lib, or $VULKAN_SDK/lib).
    if (SDL_Vulkan_LoadLibrary(HYOSHI_VULKAN_LOADER_PATH))
    {
        HYOSHI_LOG_DEBUG("Loaded Vulkan from {}", HYOSHI_VULKAN_LOADER_PATH);
        isVulkanLoaded = true;
        return {};
    }
    message += std::string("; also tried " HYOSHI_VULKAN_LOADER_PATH ": ") + SDL_GetError();
#endif

    return Error{std::move(message)};
}

void Platform::SetEventHook(EventHook hook)
{
    eventHook = std::move(hook);
}

void Platform::PumpEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (eventHook)
        {
            eventHook(event);
        }

        switch (event.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            HYOSHI_LOG_INFO("Quit requested");
            lifecycle->IsQuitRequested = true;
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            isWindowResized = isWindowResized || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
            HYOSHI_LOG_DEBUG("Window {} {}x{}", event.type == SDL_EVENT_WINDOW_RESIZED ? "resized" : "pixel size",
                             event.window.data1, event.window.data2);
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            // Held keys repeat. Repeats are marked, so gameplay counts only the real press.
            input::InputEvent inputEvent;
            inputEvent.Type = event.key.down ? input::InputEventType::KeyDown : input::InputEventType::KeyUp;
            inputEvent.HostTime = static_cast<HostTimeNs>(event.common.timestamp);
            inputEvent.DeviceId = event.key.which;
            // Android's back button does what Escape does.
            inputEvent.Code = event.key.scancode == SDL_SCANCODE_AC_BACK ? SDL_SCANCODE_ESCAPE : event.key.scancode;
            inputEvent.IsRepeat = event.key.repeat;
            inputQueue.Push(inputEvent);
            if (!event.key.repeat)
            {
                HYOSHI_LOG_TRACE("Key {} '{}' at {:.3f} ms (+{:.3f} ms to pump)", event.key.down ? "down" : "up",
                                 SDL_GetKeyName(event.key.key), TimestampMs(event), PumpDelayMs(event));
            }
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            input::InputEvent inputEvent;
            inputEvent.Type = input::InputEventType::Wheel;
            inputEvent.HostTime = static_cast<HostTimeNs>(event.common.timestamp);
            inputEvent.DeviceId = event.wheel.which;
            const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            inputEvent.Value = event.wheel.y * direction;
            inputEvent.X = event.wheel.x * direction;
            inputQueue.Push(inputEvent);
            break;
        }

        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
        {
            // Mac trackpads report fingers too, as indirect devices. Only touchscreens are touch
            // input; a trackpad already moves the pointer.
            if (SDL_GetTouchDeviceType(event.tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT)
            {
                break;
            }
            input::InputEvent inputEvent;
            inputEvent.Type = event.type == SDL_EVENT_FINGER_DOWN     ? input::InputEventType::TouchDown
                              : event.type == SDL_EVENT_FINGER_MOTION ? input::InputEventType::TouchMove
                                                                      : input::InputEventType::TouchUp;
            inputEvent.HostTime = static_cast<HostTimeNs>(event.common.timestamp);
            inputEvent.DeviceId = static_cast<uint32_t>(event.tfinger.touchID);
            inputEvent.FingerId = event.tfinger.fingerID;
            inputEvent.X = event.tfinger.x;
            inputEvent.Y = event.tfinger.y;
            inputEvent.Value = event.tfinger.pressure;
            inputQueue.Push(inputEvent);
            if (event.type != SDL_EVENT_FINGER_MOTION)
            {
                HYOSHI_LOG_TRACE("Finger {} {} at ({:.3f}, {:.3f}), {:.3f} ms (+{:.3f} ms to pump)",
                                 event.tfinger.fingerID,
                                 inputEvent.Type == input::InputEventType::TouchDown ? "down" : "up", event.tfinger.x,
                                 event.tfinger.y, TimestampMs(event), PumpDelayMs(event));
            }
            break;
        }

        // Touches also arrive as synthesized mouse events; those are skipped, since the finger
        // events above already cover them.
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
        {
            const SDL_MouseID mouse = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.which : event.button.which;
            if (mouse == SDL_TOUCH_MOUSEID || window == nullptr)
            {
                break;
            }
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(window, &width, &height);

            input::InputEvent inputEvent;
            inputEvent.HostTime = static_cast<HostTimeNs>(event.common.timestamp);
            inputEvent.DeviceId = mouse;
            if (event.type == SDL_EVENT_MOUSE_MOTION)
            {
                inputEvent.Type = input::InputEventType::PointerMove;
                inputEvent.X = event.motion.x;
                inputEvent.Y = event.motion.y;
            }
            else
            {
                inputEvent.Type =
                    event.button.down ? input::InputEventType::PointerDown : input::InputEventType::PointerUp;
                inputEvent.Code = event.button.button;
                inputEvent.X = event.button.x;
                inputEvent.Y = event.button.y;
            }
            inputEvent.X /= static_cast<float>(width > 0 ? width : 1);
            inputEvent.Y /= static_cast<float>(height > 0 ? height : 1);
            inputQueue.Push(inputEvent);
            break;
        }

        default:
            if (const char* name = WindowEventName(event.type))
            {
                HYOSHI_LOG_DEBUG("Window {}", name);
            }
            break;
        }
    }
}

void Platform::WaitForEvents(int32_t timeoutMs)
{
    if (timeoutMs < 0)
    {
        SDL_WaitEvent(nullptr);
    }
    else
    {
        SDL_WaitEventTimeout(nullptr, timeoutMs);
    }
}

bool Platform::ConsumeWindowResized()
{
    const bool wasResized = isWindowResized;
    isWindowResized = false;
    return wasResized;
}

bool Platform::IsQuitRequested() const
{
    return lifecycle->IsQuitRequested;
}

void Platform::RequestQuit()
{
    lifecycle->IsQuitRequested = true;
}

bool Platform::IsSuspended() const
{
    return lifecycle->IsSuspended;
}

HostTimeNs Platform::GetHostTimeNs() const
{
    return ReadHostClock();
}

HostTimeNs ReadHostClock()
{
    return static_cast<HostTimeNs>(SDL_GetTicksNS());
}

Result<std::vector<std::byte>> LoadFile(const std::string& path)
{
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr)
    {
        return Error{"Could not read '" + path + "': " + SDL_GetError()};
    }

    const auto* bytes = static_cast<const std::byte*>(data);
    std::vector<std::byte> contents(bytes, bytes + size);
    SDL_free(data);
    return contents;
}

Result<void> SaveFile(const std::string& path, std::span<const std::byte> contents)
{
    const std::string temporary = path + ".tmp";
    if (!SDL_SaveFile(temporary.c_str(), contents.data(), contents.size()))
    {
        return Error{"Could not write '" + temporary + "': " + SDL_GetError()};
    }
    if (!SDL_RenamePath(temporary.c_str(), path.c_str()))
    {
        const std::string error = SDL_GetError();
        SDL_RemovePath(temporary.c_str());
        return Error{"Could not replace '" + path + "': " + error};
    }
    return {};
}

Result<void> MakeDirectory(const std::string& path)
{
    if (!SDL_CreateDirectory(path.c_str()))
    {
        return Error{"Could not create '" + path + "': " + SDL_GetError()};
    }
    return {};
}

PixelSize Platform::GetWindowPixelSize() const
{
    PixelSize size;
    if (window != nullptr)
    {
        SDL_GetWindowSizeInPixels(window, &size.Width, &size.Height);
    }
    return size;
}

void Platform::SetWindowSize(int32_t width, int32_t height)
{
    if (window != nullptr)
    {
        SDL_SetWindowSize(window, width, height);
        SDL_SyncWindow(window);
    }
}

const std::string& Platform::GetUserDataPath() const
{
    return userDataPath;
}

const std::string& Platform::GetBasePath() const
{
    return basePath;
}

std::string Platform::GetAssetPath(const std::string& relative) const
{
#if defined(SDL_PLATFORM_ANDROID)
    // SDL opens relative paths from the APK's assets (after the app's internal storage).
    return relative;
#else
    return basePath + relative;
#endif
}

std::string Platform::GetSharedStoragePath() const
{
#if defined(SDL_PLATFORM_ANDROID)
    const char* path = SDL_GetAndroidExternalStoragePath();
    return path != nullptr ? std::string(path) + "/" : std::string();
#else
    return {};
#endif
}

void Platform::SetWindowIcon(const uint8_t* rgba, int32_t width, int32_t height)
{
    if (window == nullptr)
    {
        return;
    }
    // SDL copies the pixels; the surface only borrows them.
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(rgba), width * 4);
    if (surface == nullptr)
    {
        HYOSHI_LOG_WARN("Window icon: {}", SDL_GetError());
        return;
    }
    if (!SDL_SetWindowIcon(window, surface))
    {
        HYOSHI_LOG_DEBUG("Window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
}

NormalizedRect Platform::GetSafeArea() const
{
    NormalizedRect area;
    SDL_Rect safe{};
    int width = 0;
    int height = 0;
    if (window == nullptr || !SDL_GetWindowSafeArea(window, &safe) || !SDL_GetWindowSize(window, &width, &height) ||
        width <= 0 || height <= 0)
    {
        return area;
    }
    area.X = static_cast<float>(safe.x) / static_cast<float>(width);
    area.Y = static_cast<float>(safe.y) / static_cast<float>(height);
    area.Width = static_cast<float>(safe.w) / static_cast<float>(width);
    area.Height = static_cast<float>(safe.h) / static_cast<float>(height);
    return area;
}

void Platform::SetOrientation(Orientation orientation)
{
#if defined(SDL_PLATFORM_ANDROID)
    // ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE, USER_PORTRAIT, and FULL_USER: either way up
    // within the choice, following the sensor and the system's rotation lock.
    constexpr jint USER_LANDSCAPE = 11;
    constexpr jint USER_PORTRAIT = 12;
    constexpr jint FULL_USER = 13;
    const jint requested = orientation == Orientation::Landscape  ? USER_LANDSCAPE
                           : orientation == Orientation::Portrait ? USER_PORTRAIT
                                                                  : FULL_USER;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env != nullptr && activity != nullptr)
    {
        jclass type = env->GetObjectClass(activity);
        jmethodID method = env->GetMethodID(type, "setRequestedOrientation", "(I)V");
        if (method != nullptr)
        {
            env->CallVoidMethod(activity, method, requested);
        }
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(activity);
    }
    // SDL reapplies its orientation hint when it recreates the window: keep it in step.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, orientation == Orientation::Landscape ? "LandscapeLeft LandscapeRight"
                                       : orientation == Orientation::Portrait
                                           ? "Portrait PortraitUpsideDown"
                                           : "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
#else
    if (window == nullptr || orientation == Orientation::Auto)
    {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if ((height > width) == (orientation == Orientation::Portrait))
    {
        return;
    }
    if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) != 0)
    {
        SDL_RestoreWindow(window);
    }
    // The same window turned sideways, shrunk if it doesn't fit the display.
    int newWidth = height;
    int newHeight = width;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect usable{};
    if (display != 0 && SDL_GetDisplayUsableBounds(display, &usable))
    {
        const float scale = std::min({1.0f, 0.92f * static_cast<float>(usable.w) / static_cast<float>(newWidth),
                                      0.92f * static_cast<float>(usable.h) / static_cast<float>(newHeight)});
        newWidth = static_cast<int>(std::lround(static_cast<float>(newWidth) * scale));
        newHeight = static_cast<int>(std::lround(static_cast<float>(newHeight) * scale));
    }
    SetWindowSize(newWidth, newHeight);
    SDL_SetWindowPosition(window, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)),
                          static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)));
#endif
}

SDL_Window* Platform::GetNativeWindow() const
{
    return window;
}

} // namespace hyoshi::platform
