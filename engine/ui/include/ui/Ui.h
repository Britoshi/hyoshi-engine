#pragma once

#include "core/Result.h"
#include "input/InputEvent.h"
#include "renderer/SpriteBatch.h"
#include "renderer/TextRenderer.h"
#include "rhi/IRenderDevice.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hyoshi::ui
{

using hyoshi::renderer::Color;

struct Rect
{
    glm::vec2 Min{0.0f};
    glm::vec2 Max{0.0f};

    static Rect FromSize(glm::vec2 topLeft, glm::vec2 size)
    {
        return {topLeft, topLeft + size};
    }

    float Width() const
    {
        return Max.x - Min.x;
    }

    float Height() const
    {
        return Max.y - Min.y;
    }

    glm::vec2 Size() const
    {
        return Max - Min;
    }

    glm::vec2 Center() const
    {
        return (Min + Max) * 0.5f;
    }

    bool Contains(glm::vec2 point) const
    {
        return point.x >= Min.x && point.x < Max.x && point.y >= Min.y && point.y < Max.y;
    }

    Rect Inset(float amount) const
    {
        return {Min + amount, Max - amount};
    }

    Rect Inset(float horizontal, float vertical) const
    {
        return {Min + glm::vec2{horizontal, vertical}, Max - glm::vec2{horizontal, vertical}};
    }

    // Splits off a strip from one side, returning it and keeping the rest.
    Rect TakeTop(float height);
    Rect TakeBottom(float height);
    Rect TakeLeft(float width);
    Rect TakeRight(float width);
};

// Draw order. Each widget draws its background on its layer and its text one above.
namespace layers
{
constexpr int32_t BACKGROUND = 0;
constexpr int32_t HUD = 20;
constexpr int32_t PANEL = 30;
constexpr int32_t MODAL = 40;
constexpr int32_t FADE = 60;
// Over everything, scene fades included.
constexpr int32_t CURSOR = 70;
} // namespace layers

namespace colors
{
constexpr Color BACKGROUND_TOP{22, 22, 38, 255};
constexpr Color BACKGROUND_BOTTOM{8, 8, 14, 255};
constexpr Color PANEL{24, 24, 40, 240};
constexpr Color PANEL_RAISED{36, 36, 58, 255};
constexpr Color SELECTED{52, 62, 110, 255};
constexpr Color ACCENT{110, 180, 255, 255};
constexpr Color GOLD{250, 210, 90, 255};
constexpr Color TEXT{236, 236, 246, 255};
constexpr Color TEXT_DIM{150, 150, 178, 255};
constexpr Color BUTTON{46, 46, 74, 255};
constexpr Color BUTTON_HOVER{62, 62, 98, 255};
constexpr Color BUTTON_PRESSED{84, 84, 130, 255};
constexpr Color PRIMARY{60, 120, 230, 255};
constexpr Color PRIMARY_HOVER{82, 142, 245, 255};
constexpr Color PRIMARY_PRESSED{50, 100, 200, 255};
constexpr Color DIM{0, 0, 0, 170};
} // namespace colors

Color Lerp(Color a, Color b, float t);

struct Fonts
{
    hyoshi::renderer::FontId Regular = 0;
    hyoshi::renderer::FontId Bold = 0;
};

struct TextOptions
{
    float Size = 32.0f;
    Color Tint = colors::TEXT;
    hyoshi::renderer::TextAlign Align = hyoshi::renderer::TextAlign::Left;
    hyoshi::renderer::TextBaseline Baseline = hyoshi::renderer::TextBaseline::Middle;
    bool IsBold = false;
    // Longer text is shortened with an ellipsis. Zero means no limit.
    float MaxWidth = 0.0f;
};

enum class ButtonKind : uint8_t
{
    Normal,
    Primary,
    // Text only, no background until hovered.
    Flat
};

// How a text field looks (Ui::TextField).
struct TextFieldStyle
{
    // Its rounded background (else the caller draws one), and the x that clears it.
    bool HasBackground = true;
    bool HasClearButton = true;
    // Its text: the size (0 for half the field's height), color, weight, alignment, and how far in
    // from the edge it starts.
    float TextSize = 0.0f;
    Color TextColor = colors::TEXT;
    bool IsBold = false;
    hyoshi::renderer::TextAlign Align = hyoshi::renderer::TextAlign::Left;
    float Margin = 20.0f;
    // Given focus, the text is selected, so what's typed first replaces it (and Backspace clears
    // it): for a value typed anew.
    bool SelectsAllOnFocus = false;
    Color SelectionColor{110, 180, 255, 90};
};

struct ScrollState
{
    // How far the content is scrolled, and where it's gliding to.
    float Offset = 0.0f;
    float Target = 0.0f;
    // Pointer drag in progress.
    bool IsDragging = false;
    bool IsPressed = false;
    float DragStartOffset = 0.0f;
    float DragStartY = 0.0f;
    // Units per second after a drag ends, decaying (touch flicks).
    float Velocity = 0.0f;
};

// A small immediate-mode UI for menus (DESIGN.md section 17.3): widgets read the frame's input and
// draw through SpriteBatch and TextRenderer in one call. Mouse, touch (the first finger acts as
// the pointer), and keyboard queries for scenes that navigate with keys.
class Ui
{
public:
    Ui(hyoshi::renderer::TextRenderer& text, Fonts fonts);

    // Creates the rounded-corner texture.
    hyoshi::Result<void> Initialize(hyoshi::rhi::IRenderDevice& device);
    void Shutdown();

    // Reads the frame's input. Widgets draw into `sprites` until the next BeginFrame.
    void BeginFrame(std::span<const hyoshi::input::InputEvent> events, glm::vec2 canvas, float deltaSeconds,
                    hyoshi::renderer::SpriteBatch& sprites, bool isPointerCaptured, bool isKeyboardCaptured);

    glm::vec2 GetCanvas() const
    {
        return canvas;
    }

    bool IsPortrait() const
    {
        return canvas.y > canvas.x;
    }

    // The part of the canvas clear of notches and rounded corners (all of it on desktops), for
    // laying out what must stay visible. Backgrounds still fill the whole canvas.
    const Rect& GetSafeRect() const
    {
        return safeRect;
    }

    // In window coordinates normalized to 0..1, from the platform. Call before BeginFrame.
    void SetSafeArea(glm::vec2 min, glm::vec2 max)
    {
        safeMin = min;
        safeMax = max;
    }

    float GetDeltaSeconds() const
    {
        return deltaSeconds;
    }

    // Seconds since the UI started, for animation.
    float GetTime() const
    {
        return time;
    }

    // Keyboard. Pressed includes the OS's repeats of a held key.
    bool WasKeyPressed(uint32_t code) const;

    bool WasAnyKeyPressed() const
    {
        return !pressedKeys.empty();
    }

    bool IsKeyHeld(uint32_t code) const;
    bool IsControlHeld() const;

    // This frame's typed text (UTF-8), from the platform's text input. Call before BeginFrame.
    void SetTypedText(std::string typed)
    {
        typedText = std::move(typed);
    }

    // A text field has focus, so the keyboard types: scenes should leave letter keys alone, and the
    // platform's text input should be on.
    bool IsTyping() const
    {
        return focusedField != 0;
    }

    // Gives the text field with this id focus, as if tapped (a key that starts a search).
    void FocusTextField(std::string_view id);

    // Pointer, in canvas units.
    glm::vec2 GetPointer() const
    {
        return pointer;
    }

    bool WasPointerPressed() const
    {
        return isPointerPressed;
    }

    bool WasPointerReleased() const
    {
        return isPointerReleased;
    }

    bool IsPointerDown() const
    {
        return isPointerDown;
    }

    // The last pointer input came from a touchscreen.
    bool IsTouch() const
    {
        return isTouch;
    }

    // While false, widgets draw but ignore input (for what's behind a modal panel).
    void SetInputEnabled(bool isEnabled)
    {
        isInputEnabled = isEnabled;
    }

    // Widgets draw from this layer up.
    void SetLayer(int32_t layer)
    {
        baseLayer = layer;
    }

    int32_t GetLayer() const
    {
        return baseLayer;
    }

    // Drawing.
    void DrawRect(const Rect& rect, Color color, int32_t layerOffset = 0);
    void DrawRoundedRect(const Rect& rect, float radius, Color color, int32_t layerOffset = 0);
    // A vertical gradient in bands.
    void DrawGradient(const Rect& rect, Color top, Color bottom, int32_t layerOffset = 0);
    // The part of the screen background that `rect` covers, to hide what scrolls under it.
    void DrawBackground(const Rect& rect, int32_t layerOffset = 0);
    float DrawText(std::string_view text, glm::vec2 anchor, const TextOptions& options, int32_t layerOffset = 1);
    float MeasureText(std::string_view text, float size, bool isBold = false);

    // Widgets. Clicks need the press and the release on the widget, without dragging in between.
    bool Button(const Rect& rect, std::string_view label, ButtonKind kind = ButtonKind::Normal, float textSize = 34.0f);
    // A labeled on/off switch; returns true when it changed.
    bool Toggle(const Rect& rect, std::string_view label, bool& value);
    // A labeled slider with - and + buttons that move it by `step`; returns true when it changed.
    bool Slider(const Rect& rect, std::string_view label, float& value, float min, float max, float step,
                std::string_view valueText);
    // A label and a row of options, one of them selected; returns true when the choice changed.
    bool Choice(const Rect& rect, std::string_view label, std::span<const std::string_view> options, size_t& selected);
    // A one-line text field editing `value`: a click or tap gives it focus, and Enter, Escape, or a
    // click elsewhere takes it away (those keys go no further). While it has focus, typed text adds
    // to the end and Backspace takes off the last character; the other keys still reach the scene
    // (arrows to move through what the text finds). Its x clears it. Returns true when the text
    // changed.
    bool TextField(const Rect& rect, std::string& value, std::string_view placeholder, std::string_view id);
    bool TextField(const Rect& rect, std::string& value, std::string_view placeholder, std::string_view id,
                   const TextFieldStyle& style);
    // The text field with this id has focus.
    bool IsFocused(std::string_view id) const;
    // Takes focus from any text field.
    void ClearFocus()
    {
        focusedField = 0;
    }
    // Whether the pointer is over the rectangle, with input enabled.
    bool IsHovered(const Rect& rect) const;
    // A click on an area that isn't a button (list rows): the press and release inside it.
    bool WasClicked(const Rect& rect, std::string_view id);

    // Vertical scrolling for a list in `viewport` whose content is `contentHeight` tall: the wheel,
    // dragging, and flicks. Call once per frame before drawing the list.
    void Scroll(ScrollState& state, const Rect& viewport, float contentHeight);

    hyoshi::renderer::SpriteBatch& GetSprites()
    {
        return *sprites;
    }

    hyoshi::renderer::TextRenderer& GetText()
    {
        return text;
    }

    const Fonts& GetFonts() const
    {
        return fonts;
    }

private:
    uint64_t GetId(std::string_view label, const Rect& rect) const;
    // Press-and-release logic shared by buttons and clickable areas. Hovered and held report the
    // widget's state for drawing.
    bool Interact(uint64_t id, const Rect& rect, bool& isHovered, bool& isHeld);

    hyoshi::renderer::TextRenderer& text;
    Fonts fonts;
    hyoshi::rhi::IRenderDevice* device = nullptr;
    hyoshi::rhi::TextureHandle circleTexture;
    hyoshi::renderer::SpriteBatch* sprites = nullptr;

    glm::vec2 canvas{0.0f};
    glm::vec2 safeMin{0.0f};
    glm::vec2 safeMax{1.0f};
    Rect safeRect;
    float deltaSeconds = 0.0f;
    float time = 0.0f;
    int32_t baseLayer = layers::PANEL;
    bool isInputEnabled = true;

    std::vector<uint32_t> pressedKeys;
    std::vector<uint32_t> heldKeys;
    glm::vec2 pointer{-1.0f};
    glm::vec2 pressPosition{0.0f};
    bool isPointerDown = false;
    bool isPointerPressed = false;
    bool isPointerReleased = false;
    // The pointer moved far enough since the press to be a drag, not a click.
    bool isDrag = false;
    bool isTouch = false;
    // The finger acting as the pointer.
    uint64_t pointerFinger = 0;
    bool hasPointerFinger = false;
    float wheel = 0.0f;
    // The widget being pressed.
    uint64_t activeId = 0;
    // The text field with focus, or 0, and whether its text is selected (TextFieldStyle).
    uint64_t focusedField = 0;
    bool isFieldSelected = false;
    std::string typedText;
};

} // namespace hyoshi::ui
