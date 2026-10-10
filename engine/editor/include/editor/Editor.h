#pragma once

#include "app/Application.h"
#include "core/Result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hyoshi::editor
{

// A dockable editor window. The editor opens it under its name, which is also its id in the saved
// layout, and Build fills it with ImGui:: calls.
class Panel
{
public:
    explicit Panel(std::string panelName) : name(std::move(panelName))
    {
    }
    virtual ~Panel() = default;

    // The window's contents. Runs every frame while the panel is open.
    virtual void Build() = 0;

    const std::string& GetName() const
    {
        return name;
    }

private:
    std::string name;
};

// A panel and whether it shows (View menu).
struct PanelSlot
{
    std::unique_ptr<Panel> Content;
    bool IsOpen = true;
};

// What the engine's editor offers a game's editor.
class EditorServices
{
public:
    EditorServices(app::AppServices& appServices, std::vector<PanelSlot>& editorPanels)
        : App(appServices), panels(editorPanels)
    {
    }

    // The engine's systems, as the game gets them.
    app::AppServices& App;

    // Adds a panel, open at first. Panel names must be unique.
    void AddPanel(std::unique_ptr<Panel> panel);

    // Replaces the running scene after this frame, as a scene's own SwitchTo does: play mode, or a
    // view of the game's own for editing.
    void SwitchScene(std::unique_ptr<app::Scene> scene)
    {
        nextScene = std::move(scene);
    }
    // The scene SwitchScene asked for, for the editor host.
    std::unique_ptr<app::Scene> TakeNextScene()
    {
        return std::exchange(nextScene, nullptr);
    }

    // Where the game shows between the docked panels, as fractions of the window, as of the last
    // frame: a scene can lay itself out there rather than under the panels.
    platform::NormalizedRect GetGameView() const
    {
        return gameView;
    }
    void SetGameView(platform::NormalizedRect view)
    {
        gameView = view;
    }

    // Play mode: the panels hide, leaving the game the window below the menu bar.
    void SetPanelsHidden(bool isHidden)
    {
        arePanelsHidden = isHidden;
    }
    bool ArePanelsHidden() const
    {
        return arePanelsHidden;
    }

private:
    std::vector<PanelSlot>& panels;
    std::unique_ptr<app::Scene> nextScene;
    platform::NormalizedRect gameView;
    bool arePanelsHidden = false;
};

// A game's editor: the game's own panels over the running game (ADR 0002). A game implements this
// next to its app::Game, and builds both with hyoshi_add_editor.
class Editor
{
public:
    virtual ~Editor() = default;

    // After the game's Initialize: add the game's panels.
    virtual Result<void> Initialize(EditorServices& services) = 0;

    // Every frame, before the menu bar and the panels: shortcuts that work whichever panel has
    // focus, say.
    virtual void Update()
    {
    }
    // The game's items at the top of the File menu (New, Open, Save), above Quit.
    virtual void BuildFileMenu()
    {
    }
    // The game's own menus, after View.
    virtual void BuildMenus()
    {
    }
    // Once, on the first frame: a chance to dock the game's panels into the dock space (an ImGuiID)
    // with ImGui's DockBuilder, when the saved layout doesn't have them yet.
    virtual void BuildDefaultLayout([[maybe_unused]] uint32_t dockSpace)
    {
    }

    // After the last scene is gone, before the game's Shutdown.
    virtual void Shutdown()
    {
    }
};

// Runs the game under its editor until the window closes, and returns the process exit code. The
// game's first scene starts at once, without the splash.
int Run(app::AppConfig config, app::Game& game, Editor& editor);

} // namespace hyoshi::editor
