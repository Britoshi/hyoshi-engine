#pragma once

#include "app/Application.h"
#include "core/Result.h"

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

private:
    std::vector<PanelSlot>& panels;
};

// A game's editor: the game's own panels over the running game (ADR 0002). A game implements this
// next to its app::Game, and builds both with hyoshi_add_editor.
class Editor
{
public:
    virtual ~Editor() = default;

    // After the game's Initialize: add the game's panels.
    virtual Result<void> Initialize(EditorServices& services) = 0;

    // After the last scene is gone, before the game's Shutdown.
    virtual void Shutdown()
    {
    }
};

// Runs the game under its editor until the window closes, and returns the process exit code. The
// game's first scene starts at once, without the splash.
int Run(app::AppConfig config, app::Game& game, Editor& editor);

} // namespace hyoshi::editor
