#include "ui/Ui.h"

#include "renderer/Camera2D.h"

#include <doctest/doctest.h>

#include <vector>

using hyoshi::input::InputEvent;
using hyoshi::input::InputEventType;
using hyoshi::ui::Rect;

namespace
{

constexpr glm::vec2 CANVAS{1920.0f, 1080.0f};

// Headless: without a device, text measures but doesn't draw, and nothing needs the GPU.
struct UiFixture
{
    hyoshi::renderer::TextRenderer Text;
    hyoshi::renderer::SpriteBatch Sprites;
    hyoshi::renderer::Camera2D Camera;
    hyoshi::ui::Ui Gui{Text, {}};

    void Frame(std::vector<InputEvent> events, bool isKeyboardCaptured = false)
    {
        Sprites.Begin(Camera);
        Gui.BeginFrame(events, CANVAS, 1.0f / 60.0f, Sprites, false, isKeyboardCaptured);
    }
};

InputEvent Pointer(InputEventType type, glm::vec2 position)
{
    InputEvent event;
    event.Type = type;
    event.Code = 1;
    event.X = position.x / CANVAS.x;
    event.Y = position.y / CANVAS.y;
    return event;
}

InputEvent Touch(InputEventType type, glm::vec2 position, uint64_t finger)
{
    InputEvent event = Pointer(type, position);
    event.FingerId = finger;
    return event;
}

InputEvent Key(uint32_t code, bool isRepeat = false)
{
    InputEvent event;
    event.Type = InputEventType::KeyDown;
    event.Code = code;
    event.IsRepeat = isRepeat;
    return event;
}

const Rect BUTTON = Rect::FromSize({100.0f, 100.0f}, {300.0f, 100.0f});
constexpr glm::vec2 INSIDE{250.0f, 150.0f};
constexpr glm::vec2 OUTSIDE{800.0f, 800.0f};

} // namespace

TEST_CASE("A button clicks on a press and release over it")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;

    SUBCASE("press and release in separate frames")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE)});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
        fixture.Frame({Pointer(InputEventType::PointerUp, INSIDE)});
        CHECK(ui.Button(BUTTON, "Play"));
        // Only once.
        fixture.Frame({});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("a quick click within one frame")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE), Pointer(InputEventType::PointerUp, INSIDE)});
        CHECK(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("releasing elsewhere cancels")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE)});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
        fixture.Frame({Pointer(InputEventType::PointerUp, OUTSIDE)});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("pressing elsewhere and releasing over it doesn't click")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, OUTSIDE)});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
        fixture.Frame({Pointer(InputEventType::PointerMove, INSIDE), Pointer(InputEventType::PointerUp, INSIDE)});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("a drag isn't a click")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, {120.0f, 150.0f})});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
        fixture.Frame({Pointer(InputEventType::PointerMove, {380.0f, 150.0f}),
                       Pointer(InputEventType::PointerUp, {380.0f, 150.0f})});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("the right button doesn't click")
    {
        InputEvent down = Pointer(InputEventType::PointerDown, INSIDE);
        InputEvent up = Pointer(InputEventType::PointerUp, INSIDE);
        down.Code = 3;
        up.Code = 3;
        fixture.Frame({down, up});
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
    SUBCASE("disabled input ignores it")
    {
        fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE), Pointer(InputEventType::PointerUp, INSIDE)});
        ui.SetInputEnabled(false);
        CHECK_FALSE(ui.Button(BUTTON, "Play"));
    }
}

TEST_CASE("Touch: the first finger is the pointer, and nothing stays hovered after it lifts")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;

    fixture.Frame({Touch(InputEventType::TouchDown, INSIDE, 7)});
    CHECK(ui.IsTouch());
    CHECK(ui.IsHovered(BUTTON));
    CHECK_FALSE(ui.Button(BUTTON, "Play"));
    // A second finger elsewhere doesn't move the pointer.
    fixture.Frame({Touch(InputEventType::TouchDown, OUTSIDE, 8), Touch(InputEventType::TouchUp, INSIDE, 7)});
    CHECK(ui.Button(BUTTON, "Play"));
    fixture.Frame({});
    CHECK_FALSE(ui.IsHovered(BUTTON));
}

TEST_CASE("Trackpad fingers are positions on the pad, not the window: menus ignore them")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;

    fixture.Frame({Pointer(InputEventType::PointerMove, OUTSIDE)});
    fixture.Frame({Touch(InputEventType::TrackpadDown, INSIDE, 7), Touch(InputEventType::TrackpadMove, INSIDE, 7),
                   Touch(InputEventType::TrackpadUp, INSIDE, 7)});
    CHECK_FALSE(ui.IsTouch());
    CHECK_FALSE(ui.IsHovered(BUTTON));
    CHECK_FALSE(ui.Button(BUTTON, "Play"));
}

TEST_CASE("A clickable area keeps its identity while it moves")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;

    fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE)});
    CHECK_FALSE(ui.WasClicked(BUTTON, "row 3"));
    // The list glided a few units between press and release.
    const Rect moved = Rect::FromSize(BUTTON.Min + glm::vec2(0.0f, 6.0f), BUTTON.Size());
    fixture.Frame({Pointer(InputEventType::PointerUp, INSIDE)});
    CHECK(ui.WasClicked(moved, "row 3"));
}

TEST_CASE("Toggles and sliders change their values")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;

    bool isOn = false;
    fixture.Frame({Pointer(InputEventType::PointerDown, INSIDE), Pointer(InputEventType::PointerUp, INSIDE)});
    CHECK(ui.Toggle(BUTTON, "Autoplay", isOn));
    CHECK(isOn);

    // The slider's track spans its lower part between the - and + buttons (58 units, each with
    // 12 of margin, and the track inset 22 more); pressing at three quarters sets three quarters
    // of the range, snapped to the step.
    const Rect slider = Rect::FromSize({0.0f, 0.0f}, {1000.0f, 100.0f});
    float value = 0.0f;
    fixture.Frame({});
    ui.Slider(slider, "Volume", value, 0.0f, 1.0f, 0.05f, "");
    const float trackLeft = 70.0f + 22.0f;
    const float trackRight = 1000.0f - 70.0f - 22.0f;
    const glm::vec2 threeQuarters{trackLeft + ((trackRight - trackLeft) * 0.75f), 75.0f};
    fixture.Frame({Pointer(InputEventType::PointerDown, threeQuarters)});
    CHECK(ui.Slider(slider, "Volume", value, 0.0f, 1.0f, 0.05f, ""));
    CHECK(value == doctest::Approx(0.75f));

    // The + button steps it.
    fixture.Frame({Pointer(InputEventType::PointerUp, threeQuarters)});
    ui.Slider(slider, "Volume", value, 0.0f, 1.0f, 0.05f, "");
    const glm::vec2 plus{1000.0f - 36.0f, 75.0f};
    fixture.Frame({Pointer(InputEventType::PointerDown, plus), Pointer(InputEventType::PointerUp, plus)});
    CHECK(ui.Slider(slider, "Volume", value, 0.0f, 1.0f, 0.05f, ""));
    CHECK(value == doctest::Approx(0.8f));
}

TEST_CASE("Keys: repeats count for menus, and nothing while the debug UI has the keyboard")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;
    namespace keys = hyoshi::input::keys;

    fixture.Frame({Key(keys::DOWN, true)});
    CHECK(ui.WasKeyPressed(keys::DOWN));
    CHECK_FALSE(ui.WasKeyPressed(keys::UP));
    fixture.Frame({});
    CHECK_FALSE(ui.WasKeyPressed(keys::DOWN));
    fixture.Frame({Key(keys::RETURN)}, true);
    CHECK_FALSE(ui.WasKeyPressed(keys::RETURN));
}

TEST_CASE("Scrolling: the wheel glides, dragging follows the pointer, and both stay in range")
{
    UiFixture fixture;
    hyoshi::ui::Ui& ui = fixture.Gui;
    const Rect viewport = Rect::FromSize({0.0f, 0.0f}, {800.0f, 1000.0f});
    hyoshi::ui::ScrollState scroll;

    InputEvent wheel;
    wheel.Type = InputEventType::Wheel;
    wheel.Value = -2.0f;
    fixture.Frame({Pointer(InputEventType::PointerMove, {400.0f, 500.0f}), wheel});
    ui.Scroll(scroll, viewport, 3000.0f);
    CHECK(scroll.Target == doctest::Approx(240.0f));
    CHECK(scroll.Offset > 0.0f);
    CHECK(scroll.Offset < scroll.Target);
    for (int i = 0; i < 120; ++i)
    {
        fixture.Frame({});
        ui.Scroll(scroll, viewport, 3000.0f);
    }
    CHECK(scroll.Offset == doctest::Approx(240.0f).epsilon(0.01));

    // Dragging up by 300 scrolls down by 300.
    fixture.Frame({Pointer(InputEventType::PointerDown, {400.0f, 800.0f})});
    ui.Scroll(scroll, viewport, 3000.0f);
    fixture.Frame({Pointer(InputEventType::PointerMove, {400.0f, 500.0f})});
    ui.Scroll(scroll, viewport, 3000.0f);
    CHECK(scroll.IsDragging);
    CHECK(scroll.Offset == doctest::Approx(540.0f));

    // Never past the end.
    fixture.Frame({Pointer(InputEventType::PointerMove, {400.0f, -5000.0f})});
    ui.Scroll(scroll, viewport, 3000.0f);
    CHECK(scroll.Offset == doctest::Approx(2000.0f));
    fixture.Frame({Pointer(InputEventType::PointerUp, {400.0f, -5000.0f})});
    ui.Scroll(scroll, viewport, 3000.0f);
    CHECK_FALSE(scroll.IsDragging);
}
