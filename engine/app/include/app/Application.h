#pragma once

#include "app/Scene.h"

#include "audio/IAudioBackend.h"
#include "core/JobSystem.h"
#include "core/Result.h"
#include "debug/DebugUi.h"
#include "input/InputEvent.h"
#include "platform/Platform.h"
#include "renderer/Camera2D.h"
#include "renderer/SpriteBatch.h"
#include "renderer/TextRenderer.h"
#include "rhi/IRenderDevice.h"
#include "ui/Logo.h"
#include "ui/Ui.h"

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace hyoshi::app
{

class ClockDemo;

// What a game tells the engine about itself.
struct AppConfig
{
    // The window title, and the name of the user data folder (settings, caches).
    std::string Name = "Hyoshi";
    // A window title other than Name, such as the editor's, which shares the game's user data.
    std::string WindowTitle;
    // The window icon on desktops: an asset path such as "textures/app-icon.png", or empty.
    std::string WindowIconPath;
    // The "Made with Hyoshi Engine" splash before the game's first scene.
    bool ShowSplash = true;
};

// The engine's systems, for a game and its scenes. They live as long as the Application.
struct AppServices
{
    platform::Platform& Platform;
    rhi::IRenderDevice& Device;
    audio::IAudioBackend& Audio;
    JobSystem& Jobs;
    // Running under the game's editor (ADR 0002): skip what suits only players, such as turning the
    // window to the saved orientation.
    bool IsEditor = false;
};

// A game plugs into the Application through this interface.
class Game
{
public:
    virtual ~Game() = default;

    // Once the window, the GPU, and audio are up: read settings, start loading content.
    virtual Result<void> Initialize(AppServices& services) = 0;

    // The game's first scene, shown after the splash. Called once.
    virtual std::unique_ptr<Scene> CreateFirstScene() = 0;

    // Every frame, before the scene runs.
    virtual void Update()
    {
    }

    // The app is going into the background, where Android may end it, or is closing: save.
    virtual void Save()
    {
    }

    // After the last scene is gone, before the engine shuts down.
    virtual void Shutdown()
    {
    }
};

// The editor's part of the main loop (ADR 0002). engine/editor implements it; a game implements
// editor::Editor instead. With one, ImGui is always on, with docking and its own saved layout.
class EditorLayer
{
public:
    virtual ~EditorLayer() = default;

    // After the game's Initialize.
    virtual Result<void> Initialize(AppServices& services) = 0;

    // Every frame after the scene ran, between ImGui's BeginFrame and Render, before the debug
    // overlay's windows so that they can dock.
    virtual void Build() = 0;

    // After the last scene is gone, before the game's Shutdown.
    virtual void Shutdown()
    {
    }
};

// The main loop: the window and its surface, the GPU, audio, input, the frame, scenes with a fade
// between them, the debug overlay (F1), and the development hooks read from HYOSHI_* environment
// variables (screenshots, timed exit, stress tests, the clock demo). A debug run exits with a
// failure code when Vulkan validation complained, song time went backwards, or a scene reported a
// failed check.
class Application
{
public:
    // With an editor layer, the game runs under its editor (engine/editor).
    Application(AppConfig config, Game& game, EditorLayer* editorLayer = nullptr);

    // Runs until the window closes. Returns the process exit code.
    int Run();

private:
    // Renderer stress test (HYOSHI_SPRITES): bouncing, spinning sprites.
    struct DemoSprite
    {
        glm::vec2 Position;
        glm::vec2 Velocity;
        float Size;
        float Rotation;
        float Spin;
        renderer::Color Tint;
        bool IsTextured;
    };

    Result<void> Initialize();
    Result<void> CreateDemoTexture();
    void LoadFonts();
    void SetWindowIcon();
    void StartFirstScene();
    void Shutdown();
    void SpawnDemoSprites(uint32_t count);
    void Update(float deltaSeconds);
    void RunScene(HostTimeNs now, float deltaSeconds);
    // Ends the current scene, keeping what the exit checks need, and hands it back to be destroyed
    // once nothing refers to it.
    std::unique_ptr<Scene> EndScene();
    void RenderFrame(float deltaSeconds);
    float GetAverageFrameMs() const;
    void BuildStatsWindow();
    void OnLifecycleEvent(platform::LifecycleEvent event);
    // The music of whichever scene is running, if any.
    songs::SongPlayer* GetActivePlayer();

    AppConfig config;
    Game& game;
    EditorLayer* editorLayer;
    platform::Platform platform;
    std::unique_ptr<rhi::IRenderDevice> device;
    renderer::SpriteBatch spriteBatch;
    renderer::TextRenderer text;
    renderer::Camera2D camera;
    debug::DebugUi debugUi;
    JobSystem jobs;
    std::unique_ptr<audio::IAudioBackend> audio;
    std::unique_ptr<ui::Ui> ui;
    std::unique_ptr<AppServices> services;
    // The engine's logos, for the splash.
    ui::Logo stackedLogo;
    ui::Logo horizontalLogo;
    std::unique_ptr<Scene> scene;
    // The scene switched away from this frame. Its sprites are still in the frame's batch and may
    // use its textures, so it lives until the frame is recorded.
    std::unique_ptr<Scene> endedScene;
    // The current scene, when it is the clock demo (HYOSHI_SCENE=clock), for its stress hook.
    ClockDemo* clockDemo = nullptr;
    std::vector<input::InputEvent> inputEvents;
    bool isAudioSuspended = false;
    // F1 shows the debug overlay (ImGui).
    bool isDebugVisible = false;
    // Fades in each new scene.
    float sceneFade = 1.0f;

    // What ended scenes leave for the exit checks.
    bool hasFailedCheck = false;
    uint32_t backwardSteps = 0;

    rhi::TextureHandle circleTexture;
    std::vector<DemoSprite> demoSprites;
    std::mt19937 random{12345};

    static constexpr size_t FRAME_TIME_HISTORY = 240;
    std::array<float, FRAME_TIME_HISTORY> frameTimesMs{};
    size_t frameTimeCursor = 0;
    size_t frameTimeSamples = 0;
};

} // namespace hyoshi::app
