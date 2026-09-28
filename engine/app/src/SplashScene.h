#pragma once

#include "app/Scene.h"
#include "ui/Logo.h"

#include <functional>
#include <memory>

namespace hyoshi::app
{

// "Made with Hyoshi Engine": the engine's logo for about two seconds, then the game's first
// scene. Any key or tap skips it.
class SplashScene : public Scene
{
public:
    using NextScene = std::function<std::unique_ptr<Scene>()>;

    SplashScene(const ui::Logo& stacked, const ui::Logo& horizontal, NextScene next);

    void RunFrame(const FrameContext& frame, ui::Ui& ui) override;

private:
    const ui::Logo& stacked;
    const ui::Logo& horizontal;
    NextScene next;
    float age = 0.0f;
    float leaveAge = -1.0f;
};

} // namespace hyoshi::app
