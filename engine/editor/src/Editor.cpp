#include "editor/Editor.h"

#include "core/Log.h"
#include "platform/Platform.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace hyoshi::editor
{

namespace
{

// The Application's view of the editor: the menu bar, the dock space, and the panels.
class EditorHost : public app::EditorLayer
{
public:
    explicit EditorHost(Editor& gameEditor) : editor(gameEditor)
    {
    }

    Result<void> Initialize(app::AppServices& appServices) override
    {
        app = &appServices;
        services = std::make_unique<EditorServices>(appServices, panels);
        if (Result<void> result = editor.Initialize(*services); !result)
        {
            return result;
        }
        isInitialized = true;
        HYOSHI_LOG_INFO("Editor: {} panels", panels.size());
        return {};
    }

    void Build() override
    {
        BuildMenuBar();
        // Over the whole window below the menu bar. The empty center shows the game and passes
        // input through to it.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        for (PanelSlot& slot : panels)
        {
            if (!slot.IsOpen)
            {
                continue;
            }
            if (ImGui::Begin(slot.Content->GetName().c_str(), &slot.IsOpen))
            {
                slot.Content->Build();
            }
            ImGui::End();
        }
    }

    void Shutdown() override
    {
        if (isInitialized)
        {
            editor.Shutdown();
            isInitialized = false;
        }
        panels.clear();
    }

private:
    void BuildMenuBar()
    {
        if (!ImGui::BeginMainMenuBar())
        {
            return;
        }
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Quit"))
            {
                app->Platform.RequestQuit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            for (PanelSlot& slot : panels)
            {
                ImGui::MenuItem(slot.Content->GetName().c_str(), nullptr, &slot.IsOpen);
            }
            ImGui::Separator();
            ImGui::TextDisabled("F1: debug overlay");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    Editor& editor;
    app::AppServices* app = nullptr;
    std::vector<PanelSlot> panels;
    std::unique_ptr<EditorServices> services;
    bool isInitialized = false;
};

} // namespace

void EditorServices::AddPanel(std::unique_ptr<Panel> panel)
{
    const bool isTaken = std::any_of(panels.begin(), panels.end(), [&](const PanelSlot& slot)
                                     { return slot.Content->GetName() == panel->GetName(); });
    if (isTaken)
    {
        HYOSHI_LOG_ERROR("Editor: a panel named '{}' already exists", panel->GetName());
        return;
    }
    panels.push_back({std::move(panel), true});
}

int Run(app::AppConfig config, app::Game& game, Editor& editor)
{
    config.ShowSplash = false;
    EditorHost host(editor);
    app::Application application(std::move(config), game, &host);
    return application.Run();
}

} // namespace hyoshi::editor
