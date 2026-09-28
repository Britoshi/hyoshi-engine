#include "SplashScene.h"

#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hyoshi::app
{

namespace
{

constexpr float FADE_IN_SECONDS = 0.7f;
constexpr float LEAVE_AT_SECONDS = 2.1f;
constexpr float FADE_OUT_SECONDS = 0.45f;
// Input this early is probably still the click that launched the game.
constexpr float SKIP_AFTER_SECONDS = 0.2f;
// The line under the logo sweeps across between these times.
constexpr float LINE_START_SECONDS = 0.35f;
constexpr float LINE_END_SECONDS = 1.1f;

float EaseOut(float t)
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - ((1.0f - clamped) * (1.0f - clamped) * (1.0f - clamped));
}

} // namespace

SplashScene::SplashScene(const ui::Logo& stackedLogo, const ui::Logo& horizontalLogo, NextScene nextScene)
    : stacked(stackedLogo), horizontal(horizontalLogo), next(std::move(nextScene))
{
}

void SplashScene::RunFrame(const FrameContext& frame, ui::Ui& ui)
{
    age += frame.DeltaSeconds;
    const bool isSkipped = age >= SKIP_AFTER_SECONDS && (ui.WasPointerPressed() || ui.WasAnyKeyPressed());
    if (leaveAge < 0.0f && (age >= LEAVE_AT_SECONDS || isSkipped))
    {
        leaveAge = age;
    }
    const float leaving = leaveAge < 0.0f ? 0.0f : std::clamp((age - leaveAge) / FADE_OUT_SECONDS, 0.0f, 1.0f);
    if (leaving >= 1.0f && next)
    {
        SwitchTo(std::exchange(next, nullptr)());
    }

    const glm::vec2 canvas = ui.GetCanvas();
    ui.SetLayer(ui::layers::BACKGROUND);
    ui.DrawGradient({{0.0f, 0.0f}, canvas}, ui::colors::BACKGROUND_TOP, ui::colors::BACKGROUND_BOTTOM);
    ui.SetLayer(ui::layers::PANEL);

    const float appear = EaseOut(age / FADE_IN_SECONDS);
    const float alpha = appear * (1.0f - leaving);
    const float scale = 0.94f + (0.06f * appear) + (0.02f * leaving);

    // The stacked logo when the screen is tall, side by side when it's wide.
    const bool isPortrait = ui.IsPortrait();
    const ui::Logo& mark = isPortrait ? stacked : horizontal;
    const glm::vec2 box = isPortrait ? glm::vec2{canvas.x * 0.66f, canvas.y * 0.42f}
                                     : glm::vec2{std::min(canvas.x * 0.56f, 1100.0f), canvas.y * 0.3f};
    const glm::vec2 center{canvas.x * 0.5f, canvas.y * 0.47f};
    const glm::vec2 half = box * scale * 0.5f;
    ui::Rect logo{center - half, center + half};
    if (mark.IsLoaded())
    {
        logo = ui::DrawLogo(ui, mark, logo, renderer::WithAlpha(ui::colors::TEXT, alpha));
    }
    else
    {
        ui::TextOptions title;
        title.Size = 96.0f * scale;
        title.IsBold = true;
        title.Align = renderer::TextAlign::Center;
        title.Tint = renderer::WithAlpha(ui::colors::TEXT, alpha);
        ui.DrawText("HYOSHI ENGINE", center, title);
        logo = {center - glm::vec2(300.0f, 60.0f), center + glm::vec2(300.0f, 60.0f)};
    }

    // A judgment line sweeps under it.
    const float sweep = EaseOut((age - LINE_START_SECONDS) / (LINE_END_SECONDS - LINE_START_SECONDS));
    if (sweep > 0.0f)
    {
        const float width = logo.Width() * sweep;
        const float y = logo.Max.y + std::max(canvas.y * 0.05f, 40.0f);
        ui.DrawRoundedRect({{center.x - (width * 0.5f), y - 3.0f}, {center.x + (width * 0.5f), y + 3.0f}}, 3.0f,
                           renderer::WithAlpha(ui::colors::ACCENT, alpha));
    }

    // "Made with" above the logo: this is the engine's splash, before the game's own screens.
    ui::TextOptions madeWith;
    madeWith.Size = 28.0f;
    madeWith.Align = renderer::TextAlign::Center;
    madeWith.Tint = renderer::WithAlpha(ui::colors::TEXT_DIM, alpha);
    ui.DrawText("MADE WITH", {center.x, logo.Min.y - std::max(canvas.y * 0.05f, 40.0f)}, madeWith);
}

} // namespace hyoshi::app
