#pragma once

#include "core/Time.h"
#include "input/InputEvent.h"

#include <glm/vec2.hpp>

#include <memory>
#include <span>
#include <utility>

namespace hyoshi::songs
{
class SongPlayer;
}

namespace hyoshi::ui
{
class Ui;
}

namespace hyoshi::app
{

// DESIGN.md section 17.2: an app is a sequence of scenes, one running at a time.

struct FrameContext
{
    HostTimeNs Now = 0;
    // The frame's input in host time order.
    std::span<const input::InputEvent> Events;
    glm::vec2 Canvas{0.0f};
    float DeltaSeconds = 0.0f;
    // The debug UI has the keyboard, so gameplay ignores keys.
    bool IsKeyboardCaptured = false;
};

class Scene
{
public:
    virtual ~Scene() = default;

    // Updates and draws one frame. Widgets and sprites go through `ui`.
    virtual void RunFrame(const FrameContext& frame, ui::Ui& ui) = 0;

    // Debug windows (ImGui), built only while the debug overlay shows.
    virtual void BuildDebugWindow()
    {
    }

    // The scene's music, for app lifecycle handling and the exit checks.
    virtual songs::SongPlayer* GetPlayer()
    {
        return nullptr;
    }

    // The app is going into the background (the music pauses by itself).
    virtual void OnSuspend()
    {
    }

    // A scripted check failed (see Application).
    virtual bool HasFailedCheck() const
    {
        return false;
    }

    // Logged when the scene ends.
    virtual void LogSummary() const
    {
    }

    // The scene to switch to, if this one asked. The Application switches after the frame.
    std::unique_ptr<Scene> TakeNextScene()
    {
        return std::exchange(nextScene, nullptr);
    }

protected:
    void SwitchTo(std::unique_ptr<Scene> next)
    {
        nextScene = std::move(next);
    }

private:
    std::unique_ptr<Scene> nextScene;
};

} // namespace hyoshi::app
