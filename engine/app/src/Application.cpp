#include "app/Application.h"

#include "SplashScene.h"
#include "app/ClockDemo.h"

#include "core/Env.h"
#include "core/Log.h"
#include "renderer/Image.h"
#include "songs/SongPlayer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hyoshi::app
{

using platform::LifecycleEvent;
using renderer::Color;

namespace
{

// While nothing can be rendered (minimized, no surface), idle instead of spinning.
constexpr int32_t IDLE_WAIT_MS = 16;

constexpr float NS_PER_SECOND_F = 1.0e9f;

// A long stall (debugger, window drag) must not teleport the demo sprites.
constexpr float MAX_DELTA_SECONDS = 0.1f;

// Frame at which HYOSHI_SCREENSHOT is taken, late enough for the first swapchain images to settle.
// HYOSHI_SCREENSHOT_FRAME overrides it.
constexpr uint64_t DEFAULT_SCREENSHOT_FRAME = 30;

// HYOSHI_STRESS_SURFACE=1: every this many frames, alternately resize the window and destroy and
// recreate the surface, exercising the paths Android rotation and backgrounding take.
constexpr uint64_t STRESS_INTERVAL_FRAMES = 10;

// HYOSHI_STRESS_AUDIO=1: the audio device stops for the last part of every period, exercising
// the song clock's stall handling and the device restart path that route changes also take.
constexpr HostTimeNs AUDIO_STRESS_PERIOD_NS = 3'000 * hyoshi::NS_PER_MS;
constexpr HostTimeNs AUDIO_STRESS_STOP_NS = 200 * hyoshi::NS_PER_MS;

// HYOSHI_STRESS_SEEK=1: seek to a pseudo-random position this often.
constexpr HostTimeNs SEEK_STRESS_PERIOD_NS = 1'500 * hyoshi::NS_PER_MS;

// The engine's own assets sit under this folder in the app's assets (cmake/HyoshiApp.cmake).
constexpr const char* ENGINE_ASSETS = "engine/";

constexpr uint32_t DEFAULT_SPRITE_COUNT = 0;
constexpr int MAX_SPRITE_COUNT = 50'000;
constexpr uint32_t CIRCLE_TEXTURE_SIZE = 64;

// Where sprites spawn before the camera knows the real virtual size.
constexpr glm::vec2 SPAWN_AREA{1920.0f, 1080.0f};

#if defined(NDEBUG)
constexpr bool ENABLE_VALIDATION = false;
#else
constexpr bool ENABLE_VALIDATION = true;
#endif

// Development aids read from the environment, so scripts can drive the app.
std::optional<long long> ReadPositiveEnv(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }

    char* end = nullptr;
    const long long number = std::strtoll(value, &end, 10);
    if (end == value || number <= 0)
    {
        HYOSHI_LOG_WARN("Ignoring {}='{}'", name, value);
        return std::nullopt;
    }
    return number;
}

const char* ToString(hyoshi::rhi::SurfaceTransform transform)
{
    switch (transform)
    {
    case hyoshi::rhi::SurfaceTransform::Identity:
        return "identity";
    case hyoshi::rhi::SurfaceTransform::Rotate90:
        return "90";
    case hyoshi::rhi::SurfaceTransform::Rotate180:
        return "180";
    case hyoshi::rhi::SurfaceTransform::Rotate270:
        return "270";
    }
    return "?";
}

} // namespace

Application::Application(AppConfig appConfig, Game& appGame, EditorLayer* appEditorLayer)
    : config(std::move(appConfig)), game(appGame), editorLayer(appEditorLayer)
{
}

int Application::Run()
{
    if (hyoshi::Result<void> result = Initialize(); !result)
    {
        HYOSHI_LOG_CRITICAL("Initialization failed: {}", result.GetError().Message);
        Shutdown();
        return EXIT_FAILURE;
    }

    const std::optional<long long> exitAfterMs = ReadPositiveEnv("HYOSHI_EXIT_AFTER_MS");
    const char* screenshotPath = std::getenv("HYOSHI_SCREENSHOT");
    const auto screenshotFrame =
        static_cast<uint64_t>(ReadPositiveEnv("HYOSHI_SCREENSHOT_FRAME").value_or(DEFAULT_SCREENSHOT_FRAME));
    const bool stressSurface = std::getenv("HYOSHI_STRESS_SURFACE") != nullptr;
    const bool stressAudio = std::getenv("HYOSHI_STRESS_AUDIO") != nullptr;
    bool isAudioStressStopped = false;
    uint32_t audioStressCycles = 0;
    const HostTimeNs startTime = platform.GetHostTimeNs();
    // HYOSHI_SCREENSHOT_MS takes the screenshot at a time instead, for when the frame rate varies
    // (it is uncapped while the display sleeps).
    const std::optional<long long> screenshotMs = ReadPositiveEnv("HYOSHI_SCREENSHOT_MS");
    bool isScreenshotRequested = false;
    const bool stressSeek = std::getenv("HYOSHI_STRESS_SEEK") != nullptr;
    HostTimeNs nextStressSeek = startTime + SEEK_STRESS_PERIOD_NS;
    uint32_t seekStressCycles = 0;
    std::optional<HostTimeNs> lastFrameTime;
    uint64_t frameCount = 0;
    uint32_t stressCycles = 0;

    while (!platform.IsQuitRequested())
    {
        platform.PumpEvents();

        if (exitAfterMs && platform.GetHostTimeNs() - startTime >= *exitAfterMs * hyoshi::NS_PER_MS)
        {
            HYOSHI_LOG_INFO("HYOSHI_EXIT_AFTER_MS elapsed after {} frames ({} surface, {} audio, {} seek stress "
                            "cycles), {:.2f} ms average frame time, exiting",
                            frameCount, stressCycles, audioStressCycles, seekStressCycles, GetAverageFrameMs());
            break;
        }

        // Android destroys the native window in the background: release the surface, and recreate
        // it on return (DESIGN.md section 7).
        if (platform.IsSuspended())
        {
            if (songs::SongPlayer* player = GetActivePlayer(); player != nullptr && !isAudioSuspended)
            {
                scene->OnSuspend();
                player->OnSuspend();
                isAudioSuspended = true;
            }
            if (services)
            {
                game.Save();
            }
            // Input from before or during the suspension is stale.
            platform.GetInputQueue().Take(inputEvents);
            device->DestroySurface();
            platform.WaitForEvents();
            continue;
        }
        if (isAudioSuspended)
        {
            GetActivePlayer()->OnResume();
            isAudioSuspended = false;
        }
        if (!device->HasSurface())
        {
            if (hyoshi::Result<void> result = device->CreateSurface(); !result)
            {
                HYOSHI_LOG_ERROR("Could not recreate the surface: {}", result.GetError().Message);
                platform.WaitForEvents(IDLE_WAIT_MS);
                continue;
            }
        }

        if (platform.ConsumeWindowResized())
        {
            if (hyoshi::Result<void> result = device->RecreateSwapchain(); !result)
            {
                HYOSHI_LOG_ERROR("Swapchain recreation failed: {}", result.GetError().Message);
            }
        }

        if (stressSurface && frameCount > 0 && frameCount % STRESS_INTERVAL_FRAMES == 0)
        {
            ++stressCycles;
            if (stressCycles % 2 == 1)
            {
                platform.SetWindowSize(stressCycles % 4 == 1 ? 900 : 1280, stressCycles % 4 == 1 ? 1200 : 720);
            }
            else
            {
                device->DestroySurface();
                if (hyoshi::Result<void> result = device->CreateSurface(); !result)
                {
                    HYOSHI_LOG_ERROR("Stress: surface recreation failed: {}", result.GetError().Message);
                }
            }
        }

        const bool isScreenshotDue = screenshotMs
                                         ? platform.GetHostTimeNs() - startTime >= *screenshotMs * hyoshi::NS_PER_MS
                                         : frameCount == screenshotFrame;
        if (screenshotPath != nullptr && !isScreenshotRequested && isScreenshotDue)
        {
            device->RequestScreenshot(screenshotPath);
            isScreenshotRequested = true;
        }

        if (device->BeginFrame() == hyoshi::rhi::FrameStatus::Skipped)
        {
            platform.WaitForEvents(IDLE_WAIT_MS);
            continue;
        }

        const HostTimeNs now = platform.GetHostTimeNs();
        float deltaSeconds = 0.0f;
        if (lastFrameTime)
        {
            deltaSeconds = static_cast<float>(now - *lastFrameTime) / NS_PER_SECOND_F;
            frameTimesMs[frameTimeCursor] = deltaSeconds * 1000.0f;
            frameTimeCursor = (frameTimeCursor + 1) % FRAME_TIME_HISTORY;
            frameTimeSamples = std::min(frameTimeSamples + 1, FRAME_TIME_HISTORY);
        }
        lastFrameTime = now;

        if (stressAudio && audio)
        {
            const bool shouldStop =
                (now - startTime) % AUDIO_STRESS_PERIOD_NS >= AUDIO_STRESS_PERIOD_NS - AUDIO_STRESS_STOP_NS;
            if (shouldStop != isAudioStressStopped)
            {
                shouldStop ? audio->Suspend() : audio->Resume();
                isAudioStressStopped = shouldStop;
                audioStressCycles += shouldStop ? 1 : 0;
            }
        }

        if (stressSeek && clockDemo && clockDemo->GetPlayer()->IsLoaded() && now >= nextStressSeek)
        {
            std::uniform_int_distribution<hyoshi::SongTimeUs> position(0, clockDemo->GetPlayer()->GetEndTime());
            clockDemo->SeekTo(position(random));
            nextStressSeek = now + SEEK_STRESS_PERIOD_NS;
            ++seekStressCycles;
        }

        if (audio)
        {
            audio->Update();
        }
        if (services)
        {
            game.Update();
        }
        // Input is taken only on frames that are drawn, so none is lost to a skipped frame.
        platform.GetInputQueue().Take(inputEvents);
        for (const hyoshi::input::InputEvent& event : inputEvents)
        {
            if (event.Type == hyoshi::input::InputEventType::KeyDown && !event.IsRepeat &&
                event.Code == hyoshi::input::keys::F1)
            {
                isDebugVisible = !isDebugVisible;
            }
        }

        // BeginFrame may have recreated the swapchain.
        camera.SetTarget(device->GetSwapchainExtent(), device->GetSurfaceTransform());
        const float frameSeconds = std::min(deltaSeconds, MAX_DELTA_SECONDS);
        spriteBatch.Begin(camera);
        RunScene(now, frameSeconds);
        Update(frameSeconds);

        // ImGui runs every frame so its input doesn't pile up; windows show only with F1, or always
        // in the editor.
        debugUi.BeginFrame();
        if (editorLayer != nullptr && services)
        {
            editorLayer->Build();
        }
        if (isDebugVisible)
        {
            BuildStatsWindow();
            if (scene)
            {
                scene->BuildDebugWindow();
            }
        }

        RenderFrame(frameSeconds);
        device->EndFrameAndPresent();
        ++frameCount;
    }

    const uint32_t validationMessages = device->GetValidationMessageCount();
    EndScene();
    Shutdown();

    if (validationMessages > 0)
    {
        HYOSHI_LOG_ERROR("Vulkan validation reported {} warnings or errors", validationMessages);
        return EXIT_FAILURE;
    }
    if (backwardSteps > 0)
    {
        HYOSHI_LOG_ERROR("Song time went backwards {} times during playback", backwardSteps);
        return EXIT_FAILURE;
    }
    if (hasFailedCheck)
    {
        HYOSHI_LOG_ERROR("A scene reported a failed check (see its summary above)");
        return EXIT_FAILURE;
    }

    HYOSHI_LOG_INFO("Shut down cleanly");
    return EXIT_SUCCESS;
}

hyoshi::Result<void> Application::Initialize()
{
    platform.SetLifecycleCallback([this](LifecycleEvent event) { OnLifecycleEvent(event); });

    platform::PlatformConfig platformConfig;
    platformConfig.AppName = config.Name;
    platformConfig.WindowTitle = config.WindowTitle.empty() ? config.Name : config.WindowTitle;
    platformConfig.EnableVulkan = true;
    // HYOSHI_WINDOW_SIZE=720x1280 opens a portrait window, to check layouts on the desktop.
    if (const char* size = std::getenv("HYOSHI_WINDOW_SIZE"))
    {
        int width = 0;
        int height = 0;
        if (std::sscanf(size, "%dx%d", &width, &height) == 2 && width > 0 && height > 0)
        {
            platformConfig.WindowWidth = width;
            platformConfig.WindowHeight = height;
        }
        else
        {
            HYOSHI_LOG_WARN("Ignoring HYOSHI_WINDOW_SIZE='{}'", size);
        }
    }
    if (hyoshi::Result<void> result = platform.Initialize(platformConfig); !result)
    {
        return result;
    }

    hyoshi::rhi::DeviceConfig deviceConfig;
    deviceConfig.EnableValidation = ENABLE_VALIDATION;
    if (!platform.GetUserDataPath().empty())
    {
        deviceConfig.PipelineCachePath = platform.GetUserDataPath() + "pipeline_cache.bin";
    }

    device = hyoshi::rhi::CreateRenderDevice();
    if (hyoshi::Result<void> result = device->Initialize(deviceConfig, platform); !result)
    {
        return result;
    }

    if (hyoshi::Result<void> result = spriteBatch.Initialize(*device); !result)
    {
        return result;
    }
    // The editor's panels dock, and its layout is kept apart from the debug overlay's.
    hyoshi::debug::DebugUiConfig debugUiConfig;
    if (editorLayer != nullptr)
    {
        debugUiConfig.EnableDocking = true;
        debugUiConfig.IniFileName = "editor-imgui.ini";
    }
    if (hyoshi::Result<void> result = debugUi.Initialize(*device, platform, debugUiConfig); !result)
    {
        return result;
    }
    if (hyoshi::Result<void> result = CreateDemoTexture(); !result)
    {
        return result;
    }
    text.Initialize(*device);
    LoadFonts();
    if (hyoshi::Result<void> result = ui->Initialize(*device); !result)
    {
        return result;
    }
    const std::string engineTextures = platform.GetAssetPath(std::string(ENGINE_ASSETS) + "textures/");
    stackedLogo = ui::LoadLogo(*device, engineTextures + "logo-stacked.png", 768);
    horizontalLogo = ui::LoadLogo(*device, engineTextures + "logo-horizontal.png", 1024);
    SetWindowIcon();
    isDebugVisible = IsEnvSet("HYOSHI_DEBUG_UI");

    // Audio is optional: without an output device the app still runs, but plays nothing.
    audio = hyoshi::audio::CreateAudioBackend();
    hyoshi::audio::AudioConfig audioConfig;
    audioConfig.HostClock = &hyoshi::platform::ReadHostClock;
    if (Result<void> result = audio->Initialize(audioConfig); result)
    {
        services = std::make_unique<AppServices>(AppServices{platform, *device, *audio, jobs, editorLayer != nullptr});
        if (Result<void> gameResult = game.Initialize(*services); !gameResult)
        {
            return gameResult;
        }
        if (editorLayer != nullptr)
        {
            if (Result<void> editorResult = editorLayer->Initialize(*services); !editorResult)
            {
                return editorResult;
            }
        }
        StartFirstScene();
    }
    else
    {
        HYOSHI_LOG_WARN("Running without audio: {}", result.GetError().Message);
        audio.reset();
    }

    // HYOSHI_SPRITES=<count> sets the demo's starting sprite count, e.g. 10000 for a stress test.
    const long long spriteCount = ReadPositiveEnv("HYOSHI_SPRITES").value_or(DEFAULT_SPRITE_COUNT);
    SpawnDemoSprites(static_cast<uint32_t>(std::min<long long>(spriteCount, MAX_SPRITE_COUNT)));

    return {};
}

// An anti-aliased white disc, so textured sprites exercise sampling and alpha blending.
hyoshi::Result<void> Application::CreateDemoTexture()
{
    hyoshi::rhi::TextureDesc desc;
    desc.Width = CIRCLE_TEXTURE_SIZE;
    desc.Height = CIRCLE_TEXTURE_SIZE;
    desc.DebugName = "Demo circle";
    hyoshi::Result<hyoshi::rhi::TextureHandle> texture = device->CreateTexture(desc);
    if (!texture)
    {
        return texture.GetError();
    }
    circleTexture = texture.Value();

    std::vector<uint32_t> pixels(size_t{CIRCLE_TEXTURE_SIZE} * CIRCLE_TEXTURE_SIZE);
    const float radius = static_cast<float>(CIRCLE_TEXTURE_SIZE) * 0.5f;
    for (uint32_t y = 0; y < CIRCLE_TEXTURE_SIZE; ++y)
    {
        for (uint32_t x = 0; x < CIRCLE_TEXTURE_SIZE; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - radius;
            const float dy = static_cast<float>(y) + 0.5f - radius;
            const float coverage = std::clamp(radius - 1.0f - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            const auto alpha = static_cast<uint32_t>(std::lround(coverage * 255.0f));
            pixels[(size_t{y} * CIRCLE_TEXTURE_SIZE) + x] = 0x00FFFFFFu | (alpha << 24);
        }
    }

    return device->UpdateTexture(circleTexture, pixels.data(), 0, 0, CIRCLE_TEXTURE_SIZE, CIRCLE_TEXTURE_SIZE);
}

void Application::LoadFonts()
{
    // Installed next to the executable by the build (cmake/HyoshiApp.cmake), or in the APK's assets.
    const std::string fontFolder = platform.GetAssetPath(std::string(ENGINE_ASSETS) + "fonts/");
    auto load = [&](const char* file, std::optional<hyoshi::renderer::FontId> fallback)
    {
        hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(fontFolder + file);
        if (!bytes)
        {
            HYOSHI_LOG_ERROR("Font: {}", bytes.GetError().Message);
            return fallback.value_or(0);
        }
        hyoshi::Result<hyoshi::renderer::FontId> font = text.AddFont(std::move(bytes).Value(), fallback);
        if (!font)
        {
            HYOSHI_LOG_ERROR("Font {}: {}", file, font.GetError().Message);
            return fallback.value_or(0);
        }
        return font.Value();
    };
    ui::Fonts fonts;
    fonts.Regular = load("NotoSansJP-Regular.otf", std::nullopt);
    fonts.Bold = load("NotoSansJP-Bold.otf", fonts.Regular);
    ui = std::make_unique<ui::Ui>(text, fonts);
}

// The game's icon (hyoshi-asset-cooker makes one from a logo), on desktops.
void Application::SetWindowIcon()
{
    if (config.WindowIconPath.empty())
    {
        return;
    }
    const std::string path = platform.GetAssetPath(config.WindowIconPath);
    hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
    if (!bytes)
    {
        HYOSHI_LOG_WARN("Window icon: {}", bytes.GetError().Message);
        return;
    }
    hyoshi::Result<hyoshi::renderer::Image> icon = hyoshi::renderer::DecodeImage(bytes.Value());
    if (!icon)
    {
        HYOSHI_LOG_WARN("Window icon: {}", icon.GetError().Message);
        return;
    }
    platform.SetWindowIcon(icon.Value().Pixels.data(), static_cast<int32_t>(icon.Value().Width),
                           static_cast<int32_t>(icon.Value().Height));
}

// The splash, then the game's first scene. HYOSHI_SCENE=clock runs the clock demo instead.
void Application::StartFirstScene()
{
    const char* sceneName = std::getenv("HYOSHI_SCENE");
    if (sceneName != nullptr && std::string_view(sceneName) == "clock")
    {
        auto demo = std::make_unique<ClockDemo>(*audio, jobs);
        demo->Start();
        clockDemo = demo.get();
        scene = std::move(demo);
    }
    else if (config.ShowSplash)
    {
        scene = std::make_unique<SplashScene>(stackedLogo, horizontalLogo, [this] { return game.CreateFirstScene(); });
    }
    else
    {
        scene = game.CreateFirstScene();
    }
}

void Application::RunScene(HostTimeNs now, float deltaSeconds)
{
    const glm::vec2 canvas = camera.GetVirtualSize();
    // The editor's windows are always there; the debug overlay's only while it shows.
    const bool hasImGuiWindows = isDebugVisible || editorLayer != nullptr;
    const bool isKeyboardCaptured = hasImGuiWindows && debugUi.WantsKeyboard();
    const bool isPointerCaptured = hasImGuiWindows && debugUi.WantsPointer();
    const hyoshi::platform::NormalizedRect safe = platform.GetSafeArea();
    ui->SetSafeArea({safe.X, safe.Y}, {safe.X + safe.Width, safe.Y + safe.Height});
    ui->BeginFrame(inputEvents, canvas, deltaSeconds, spriteBatch, isPointerCaptured, isKeyboardCaptured);

    if (!scene)
    {
        ui->SetLayer(ui::layers::PANEL);
        ui::TextOptions message;
        message.Size = 40.0f;
        message.Align = hyoshi::renderer::TextAlign::Center;
        ui->DrawText("No audio output device found", canvas * 0.5f, message);
        return;
    }

    FrameContext frame;
    frame.Now = now;
    frame.Events = inputEvents;
    frame.Canvas = canvas;
    frame.DeltaSeconds = deltaSeconds;
    frame.IsKeyboardCaptured = isKeyboardCaptured;
    scene->RunFrame(frame, *ui);
    if (std::unique_ptr<Scene> next = scene->TakeNextScene())
    {
        EndScene();
        scene = std::move(next);
        sceneFade = 1.0f;
    }
}

void Application::EndScene()
{
    if (!scene)
    {
        return;
    }
    scene->LogSummary();
    hasFailedCheck = hasFailedCheck || scene->HasFailedCheck();
    if (const songs::SongPlayer* player = scene->GetPlayer())
    {
        backwardSteps += player->GetBackwardSteps();
    }
    scene.reset();
    clockDemo = nullptr;
}

void Application::Shutdown()
{
    if (services)
    {
        game.Save();
    }
    scene.reset();
    clockDemo = nullptr;
    if (services)
    {
        if (editorLayer != nullptr)
        {
            editorLayer->Shutdown();
        }
        game.Shutdown();
        services.reset();
    }
    if (audio)
    {
        audio->Shutdown();
        audio.reset();
    }

    // Everything that owns GPU resources goes before the device.
    if (ui)
    {
        ui->Shutdown();
    }
    if (device)
    {
        ui::DestroyLogo(*device, stackedLogo);
        ui::DestroyLogo(*device, horizontalLogo);
    }
    text.Shutdown();
    debugUi.Shutdown();
    spriteBatch.Shutdown();
    if (device)
    {
        device->Destroy(circleTexture);
        circleTexture = {};
        device->Shutdown();
        device.reset();
    }
    platform.Shutdown();
}

void Application::SpawnDemoSprites(uint32_t count)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_int_distribution<int> channel(64, 255);

    const size_t previous = demoSprites.size();
    demoSprites.resize(count);
    for (size_t i = previous; i < demoSprites.size(); ++i)
    {
        const float angle = unit(random) * 2.0f * std::numbers::pi_v<float>;
        const float speed = 60.0f + (unit(random) * 240.0f);

        DemoSprite& sprite = demoSprites[i];
        sprite.Position = glm::vec2(unit(random), unit(random)) * SPAWN_AREA;
        sprite.Velocity = glm::vec2(std::cos(angle), std::sin(angle)) * speed;
        sprite.Size = 8.0f + (unit(random) * 40.0f);
        sprite.Rotation = unit(random) * 2.0f * std::numbers::pi_v<float>;
        sprite.Spin = (unit(random) - 0.5f) * 4.0f;
        sprite.Tint = {static_cast<uint8_t>(channel(random)), static_cast<uint8_t>(channel(random)),
                       static_cast<uint8_t>(channel(random)), 220};
        sprite.IsTextured = i % 2 == 0;
    }
}

void Application::Update(float deltaSeconds)
{
    const glm::vec2 bounds = camera.GetVirtualSize();
    for (DemoSprite& sprite : demoSprites)
    {
        sprite.Position += sprite.Velocity * deltaSeconds;
        sprite.Rotation += sprite.Spin * deltaSeconds;

        // Bounce off the edges. Checking the direction lets sprites stranded outside by a resize
        // travel back in.
        if ((sprite.Position.x < 0.0f && sprite.Velocity.x < 0.0f) ||
            (sprite.Position.x > bounds.x && sprite.Velocity.x > 0.0f))
        {
            sprite.Velocity.x = -sprite.Velocity.x;
        }
        if ((sprite.Position.y < 0.0f && sprite.Velocity.y < 0.0f) ||
            (sprite.Position.y > bounds.y && sprite.Velocity.y > 0.0f))
        {
            sprite.Velocity.y = -sprite.Velocity.y;
        }
    }
}

void Application::RenderFrame(float deltaSeconds)
{
    hyoshi::rhi::ICommandList& commands = device->GetCommandList();

    hyoshi::rhi::RenderPassDesc renderPass;
    renderPass.Clear = {0.04f, 0.04f, 0.07f, 1.0f};
    commands.BeginRenderPass(renderPass);

    // The scene has drawn into the sprite batch already (RunScene).
    const glm::vec2 canvas = camera.GetVirtualSize();
    if (isDebugVisible)
    {
        // A frame around the virtual canvas, to check the camera covers the swapchain exactly.
        constexpr float BORDER = 4.0f;
        constexpr int32_t BORDER_LAYER = 50;
        const Color borderColor{90, 90, 120, 255};
        spriteBatch.DrawRect({0.0f, 0.0f}, {canvas.x, BORDER}, borderColor, BORDER_LAYER);
        spriteBatch.DrawRect({0.0f, canvas.y - BORDER}, {canvas.x, BORDER}, borderColor, BORDER_LAYER);
        spriteBatch.DrawRect({0.0f, 0.0f}, {BORDER, canvas.y}, borderColor, BORDER_LAYER);
        spriteBatch.DrawRect({canvas.x - BORDER, 0.0f}, {BORDER, canvas.y}, borderColor, BORDER_LAYER);
    }

    // Above the background, below everything else.
    constexpr int32_t DEMO_SPRITE_LAYER = 1;
    for (const DemoSprite& demoSprite : demoSprites)
    {
        hyoshi::renderer::Sprite sprite;
        sprite.Center = demoSprite.Position;
        sprite.Size = glm::vec2(demoSprite.Size);
        sprite.Rotation = demoSprite.Rotation;
        sprite.Tint = demoSprite.Tint;
        sprite.Layer = DEMO_SPRITE_LAYER;
        if (demoSprite.IsTextured)
        {
            sprite.Texture = circleTexture;
        }
        spriteBatch.Draw(sprite);
    }

    // Each new scene fades in from black.
    if (sceneFade > 0.0f)
    {
        spriteBatch.DrawRect({0.0f, 0.0f}, canvas, {0, 0, 0, static_cast<uint8_t>(sceneFade * 255.0f)},
                             ui::layers::FADE);
        sceneFade = std::max(0.0f, sceneFade - (deltaSeconds * 5.0f));
    }

    // Glyphs rasterized this frame go up before the sprites that use them are drawn.
    if (hyoshi::Result<void> result = text.Flush(); !result)
    {
        HYOSHI_LOG_ERROR("Text: {}", result.GetError().Message);
    }
    spriteBatch.End(commands);
    debugUi.Render(commands);
    commands.EndRenderPass();
}

float Application::GetAverageFrameMs() const
{
    if (frameTimeSamples == 0)
    {
        return 0.0f;
    }
    // Unfilled slots are zero, so summing the whole history is fine.
    float totalMs = 0.0f;
    for (const float frameMs : frameTimesMs)
    {
        totalMs += frameMs;
    }
    return totalMs / static_cast<float>(frameTimeSamples);
}

void Application::BuildStatsWindow()
{
    ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({320.0f, 0.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Stats"))
    {
        const float averageMs = GetAverageFrameMs();
        const float worstMs = *std::max_element(frameTimesMs.begin(), frameTimesMs.end());

        ImGui::Text("%.1f FPS  (%.2f ms avg, %.2f ms worst)", averageMs > 0.0f ? 1000.0f / averageMs : 0.0f,
                    static_cast<double>(averageMs), static_cast<double>(worstMs));
        ImGui::PlotLines("##frame times", frameTimesMs.data(), static_cast<int>(FRAME_TIME_HISTORY),
                         static_cast<int>(frameTimeCursor), "frame ms", 0.0f, 33.3f, {-1.0f, 60.0f});

        ImGui::Separator();
        ImGui::Text("Sprites: %u  Draw calls: %u", spriteBatch.GetLastSpriteCount(),
                    spriteBatch.GetLastDrawCallCount());

        int spriteCount = static_cast<int>(demoSprites.size());
        if (ImGui::SliderInt("Demo sprites", &spriteCount, 0, MAX_SPRITE_COUNT, "%d", ImGuiSliderFlags_Logarithmic))
        {
            SpawnDemoSprites(static_cast<uint32_t>(spriteCount));
        }

        ImGui::Separator();
        const hyoshi::rhi::Extent extent = device->GetSwapchainExtent();
        const glm::vec2 canvas = camera.GetVirtualSize();
        ImGui::Text("Swapchain: %ux%u, rotation %s", extent.Width, extent.Height,
                    ToString(device->GetSurfaceTransform()));
        ImGui::Text("Virtual canvas: %.0fx%.0f", static_cast<double>(canvas.x), static_cast<double>(canvas.y));

        const uint32_t validationMessages = device->GetValidationMessageCount();
        if (validationMessages > 0)
        {
            ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Validation messages: %u", validationMessages);
        }
        else
        {
            ImGui::Text("Validation messages: 0");
        }
    }
    ImGui::End();
}

// Runs on whichever thread the OS reports from. Logging only for now; the planned responses are
// in DESIGN.md section 7.
void Application::OnLifecycleEvent(LifecycleEvent event)
{
    HYOSHI_LOG_INFO("Lifecycle: {}", hyoshi::platform::ToString(event));
}

songs::SongPlayer* Application::GetActivePlayer()
{
    return scene ? scene->GetPlayer() : nullptr;
}

} // namespace hyoshi::app
