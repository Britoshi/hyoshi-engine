#include "ui/Ui.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace hyoshi::ui
{

using hyoshi::input::InputEventType;

namespace
{

// The rounded-corner texture: a circle's distance field.
constexpr uint32_t CIRCLE_SIZE = 64;
constexpr float CIRCLE_RADIUS = 28.0f;
constexpr float CIRCLE_SPREAD = 4.0f;

// The pointer moving this far after a press makes it a drag, which isn't a click.
constexpr float DRAG_THRESHOLD = 14.0f;
constexpr float WHEEL_STEP = 120.0f;
// Scrolling glides toward its target at this rate per second, and flicks slow down at this rate.
constexpr float SCROLL_SMOOTHING = 16.0f;
constexpr float FLICK_DECAY = 3.5f;
constexpr int GRADIENT_BANDS = 24;

constexpr int32_t TEXT_LAYER = 2;

uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

} // namespace

Color Lerp(Color a, Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](uint8_t x, uint8_t y)
    { return static_cast<uint8_t>(std::lround(static_cast<float>(x) + ((static_cast<float>(y) - x) * t))); };
    return {mix(a.R, b.R), mix(a.G, b.G), mix(a.B, b.B), mix(a.A, b.A)};
}

void Ui::DrawBackground(const Rect& rect, int32_t layerOffset)
{
    DrawGradient(rect, Lerp(colors::BACKGROUND_TOP, colors::BACKGROUND_BOTTOM, rect.Min.y / canvas.y),
                 Lerp(colors::BACKGROUND_TOP, colors::BACKGROUND_BOTTOM, rect.Max.y / canvas.y), layerOffset);
}

Rect Rect::TakeTop(float height)
{
    height = std::clamp(height, 0.0f, Height());
    const Rect top{Min, {Max.x, Min.y + height}};
    Min.y += height;
    return top;
}

Rect Rect::TakeBottom(float height)
{
    height = std::clamp(height, 0.0f, Height());
    const Rect bottom{{Min.x, Max.y - height}, Max};
    Max.y -= height;
    return bottom;
}

Rect Rect::TakeLeft(float width)
{
    width = std::clamp(width, 0.0f, Width());
    const Rect left{Min, {Min.x + width, Max.y}};
    Min.x += width;
    return left;
}

Rect Rect::TakeRight(float width)
{
    width = std::clamp(width, 0.0f, Width());
    const Rect right{{Max.x - width, Min.y}, Max};
    Max.x -= width;
    return right;
}

Ui::Ui(hyoshi::renderer::TextRenderer& textRenderer, Fonts fontIds) : text(textRenderer), fonts(fontIds)
{
}

hyoshi::Result<void> Ui::Initialize(hyoshi::rhi::IRenderDevice& renderDevice)
{
    device = &renderDevice;

    hyoshi::rhi::TextureDesc desc;
    desc.Width = CIRCLE_SIZE;
    desc.Height = CIRCLE_SIZE;
    desc.Format = hyoshi::rhi::TextureFormat::R8Unorm;
    desc.DebugName = "UI circle";
    hyoshi::Result<hyoshi::rhi::TextureHandle> texture = device->CreateTexture(desc);
    if (!texture)
    {
        return texture.GetError();
    }
    circleTexture = texture.Value();

    // 0.5 on the circle's edge, rising inward and falling outward over CIRCLE_SPREAD texels.
    std::vector<uint8_t> texels(size_t{CIRCLE_SIZE} * CIRCLE_SIZE);
    const float center = static_cast<float>(CIRCLE_SIZE) * 0.5f;
    for (uint32_t y = 0; y < CIRCLE_SIZE; ++y)
    {
        for (uint32_t x = 0; x < CIRCLE_SIZE; ++x)
        {
            const float distance =
                glm::length(glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) - center);
            const float value = std::clamp(0.5f + ((CIRCLE_RADIUS - distance) / (2.0f * CIRCLE_SPREAD)), 0.0f, 1.0f);
            texels[(size_t{y} * CIRCLE_SIZE) + x] = static_cast<uint8_t>(std::lround(value * 255.0f));
        }
    }
    return device->UpdateTexture(circleTexture, texels.data(), 0, 0, CIRCLE_SIZE, CIRCLE_SIZE);
}

void Ui::Shutdown()
{
    if (device != nullptr)
    {
        device->Destroy(circleTexture);
    }
    circleTexture = {};
    device = nullptr;
}

void Ui::BeginFrame(std::span<const hyoshi::input::InputEvent> events, glm::vec2 canvasSize, float frameSeconds,
                    hyoshi::renderer::SpriteBatch& batch, bool isPointerCaptured, bool isKeyboardCaptured)
{
    sprites = &batch;
    canvas = canvasSize;
    safeRect = {safeMin * canvas, safeMax * canvas};
    deltaSeconds = frameSeconds;
    time += frameSeconds;
    baseLayer = layers::PANEL;
    isInputEnabled = true;

    pressedKeys.clear();
    wheel = 0.0f;
    isPointerPressed = false;
    // A press ends the previous gesture; a widget still marked active lost its release.
    if (isPointerReleased || !isPointerDown)
    {
        activeId = 0;
    }
    isPointerReleased = false;
    // A lifted finger leaves nothing hovered.
    if (isTouch && !isPointerDown)
    {
        pointer = glm::vec2(-1.0e6f);
    }

    auto press = [this]
    {
        isPointerDown = true;
        isPointerPressed = true;
        pressPosition = pointer;
        isDrag = false;
    };
    auto release = [this]
    {
        if (isPointerDown)
        {
            isPointerDown = false;
            isPointerReleased = true;
            isDrag = isDrag || glm::distance(pointer, pressPosition) > DRAG_THRESHOLD;
        }
    };

    for (const hyoshi::input::InputEvent& event : events)
    {
        const glm::vec2 position{event.X * canvas.x, event.Y * canvas.y};
        switch (event.Type)
        {
        case InputEventType::KeyDown:
            if (!isKeyboardCaptured)
            {
                pressedKeys.push_back(event.Code);
            }
            if (!IsKeyHeld(event.Code))
            {
                heldKeys.push_back(event.Code);
            }
            break;
        case InputEventType::KeyUp:
            std::erase(heldKeys, event.Code);
            break;
        case InputEventType::PointerMove:
            if (!hasPointerFinger)
            {
                pointer = position;
                isTouch = false;
            }
            break;
        case InputEventType::PointerDown:
            if (!hasPointerFinger && event.Code == 1 && !isPointerCaptured)
            {
                pointer = position;
                isTouch = false;
                press();
            }
            break;
        case InputEventType::PointerUp:
            if (!hasPointerFinger && event.Code == 1)
            {
                pointer = position;
                release();
            }
            break;
        case InputEventType::TouchDown:
            if (!hasPointerFinger)
            {
                hasPointerFinger = true;
                pointerFinger = event.FingerId;
                pointer = position;
                isTouch = true;
                press();
            }
            break;
        case InputEventType::TouchMove:
            if (hasPointerFinger && event.FingerId == pointerFinger)
            {
                pointer = position;
            }
            break;
        case InputEventType::TouchUp:
            if (hasPointerFinger && event.FingerId == pointerFinger)
            {
                pointer = position;
                release();
                hasPointerFinger = false;
            }
            break;
        case InputEventType::Wheel:
            if (!isPointerCaptured)
            {
                wheel += event.Value;
            }
            break;
        default:
            break;
        }
    }
    if (isPointerDown && glm::distance(pointer, pressPosition) > DRAG_THRESHOLD)
    {
        isDrag = true;
    }
    if (isPointerCaptured && !isPointerDown)
    {
        pointer = glm::vec2(-1.0e6f);
    }
}

bool Ui::WasKeyPressed(uint32_t code) const
{
    return std::find(pressedKeys.begin(), pressedKeys.end(), code) != pressedKeys.end();
}

bool Ui::IsKeyHeld(uint32_t code) const
{
    return std::find(heldKeys.begin(), heldKeys.end(), code) != heldKeys.end();
}

bool Ui::IsControlHeld() const
{
    return IsKeyHeld(hyoshi::input::keys::LEFT_CTRL) || IsKeyHeld(hyoshi::input::keys::RIGHT_CTRL);
}

void Ui::DrawRect(const Rect& rect, Color color, int32_t layerOffset)
{
    sprites->DrawRect(rect.Min, rect.Size(), color, baseLayer + layerOffset);
}

void Ui::DrawRoundedRect(const Rect& rect, float radius, Color color, int32_t layerOffset)
{
    radius = std::min({radius, rect.Width() * 0.5f, rect.Height() * 0.5f});
    if (radius < 1.0f || !circleTexture.IsValid())
    {
        DrawRect(rect, color, layerOffset);
        return;
    }

    const int32_t layer = baseLayer + layerOffset;
    auto quad = [&](glm::vec2 min, glm::vec2 max, glm::vec4 uv)
    {
        hyoshi::renderer::Sprite sprite;
        sprite.Center = (min + max) * 0.5f;
        sprite.Size = max - min;
        sprite.UvRect = uv;
        sprite.Texture = circleTexture;
        sprite.Tint = color;
        sprite.Layer = layer;
        sprite.IsDistanceField = true;
        sprites->Draw(sprite);
    };

    // Nine slices: the circle's quarters make the corners, and its middle row and column,
    // stretched, make the edges.
    const float edge = ((static_cast<float>(CIRCLE_SIZE) * 0.5f) - CIRCLE_RADIUS) / static_cast<float>(CIRCLE_SIZE);
    const float mid = 0.5f;
    const float far = 1.0f - edge;
    const float x0 = rect.Min.x;
    const float x1 = x0 + radius;
    const float x3 = rect.Max.x;
    const float x2 = x3 - radius;
    const float y0 = rect.Min.y;
    const float y1 = y0 + radius;
    const float y3 = rect.Max.y;
    const float y2 = y3 - radius;
    quad({x0, y0}, {x1, y1}, {edge, edge, mid, mid});
    quad({x2, y0}, {x3, y1}, {mid, edge, far, mid});
    quad({x0, y2}, {x1, y3}, {edge, mid, mid, far});
    quad({x2, y2}, {x3, y3}, {mid, mid, far, far});
    if (x2 > x1)
    {
        quad({x1, y0}, {x2, y1}, {mid, edge, mid, mid});
        quad({x1, y2}, {x2, y3}, {mid, mid, mid, far});
    }
    if (y2 > y1)
    {
        quad({x0, y1}, {x1, y2}, {edge, mid, mid, mid});
        quad({x2, y1}, {x3, y2}, {mid, mid, far, mid});
    }
    if (x2 > x1 && y2 > y1)
    {
        sprites->DrawRect({x1, y1}, {x2 - x1, y2 - y1}, color, layer);
    }
}

void Ui::DrawGradient(const Rect& rect, Color top, Color bottom, int32_t layerOffset)
{
    const float band = rect.Height() / static_cast<float>(GRADIENT_BANDS);
    for (int i = 0; i < GRADIENT_BANDS; ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(GRADIENT_BANDS);
        // Overlap by a unit so no seam shows between bands.
        DrawRect(Rect::FromSize({rect.Min.x, rect.Min.y + (band * static_cast<float>(i))}, {rect.Width(), band + 1.0f}),
                 Lerp(top, bottom, t), layerOffset);
    }
}

float Ui::DrawText(std::string_view str, glm::vec2 anchor, const TextOptions& options, int32_t layerOffset)
{
    hyoshi::renderer::TextStyle style;
    style.Font = options.IsBold ? fonts.Bold : fonts.Regular;
    style.Size = options.Size;
    style.Tint = options.Tint;
    style.Align = options.Align;
    style.Baseline = options.Baseline;
    style.Layer = baseLayer + layerOffset;
    if (options.MaxWidth > 0.0f)
    {
        const std::string shortened = text.Elide(str, style.Font, options.Size, options.MaxWidth);
        return text.Draw(*sprites, shortened, anchor, style);
    }
    return text.Draw(*sprites, str, anchor, style);
}

float Ui::MeasureText(std::string_view str, float size, bool isBold)
{
    return text.MeasureWidth(str, isBold ? fonts.Bold : fonts.Regular, size);
}

uint64_t Ui::GetId(std::string_view label, const Rect& rect) const
{
    uint64_t hash = 14695981039346656037ull;
    hash = HashBytes(hash, label.data(), label.size());
    const std::array<int32_t, 3> position{static_cast<int32_t>(std::lround(rect.Min.x)),
                                          static_cast<int32_t>(std::lround(rect.Min.y)), baseLayer};
    return HashBytes(hash, position.data(), sizeof(position));
}

bool Ui::IsHovered(const Rect& rect) const
{
    // A touchscreen only "hovers" while the finger is down.
    const bool hasContact = !isTouch || isPointerDown || isPointerReleased;
    return isInputEnabled && hasContact && rect.Contains(pointer);
}

bool Ui::Interact(uint64_t id, const Rect& rect, bool& isHovered, bool& isHeld)
{
    isHovered = IsHovered(rect);
    if (isHovered && isPointerPressed)
    {
        activeId = id;
    }
    bool isClicked = false;
    if (activeId == id && isPointerReleased)
    {
        isClicked = isHovered && !isDrag;
        activeId = 0;
    }
    isHeld = activeId == id && isPointerDown && isHovered && !isDrag;
    return isClicked;
}

bool Ui::WasClicked(const Rect& rect, std::string_view id)
{
    // The id alone, not the position: list rows move while the list glides.
    bool isHovered = false;
    bool isHeld = false;
    return Interact(HashBytes(14695981039346656037ull, id.data(), id.size()), rect, isHovered, isHeld);
}

bool Ui::Button(const Rect& rect, std::string_view label, ButtonKind kind, float textSize)
{
    bool isHovered = false;
    bool isHeld = false;
    const bool isClicked = Interact(GetId(label, rect), rect, isHovered, isHeld);

    Color fill{0, 0, 0, 0};
    Color textColor = colors::TEXT;
    switch (kind)
    {
    case ButtonKind::Primary:
        fill = isHeld ? colors::PRIMARY_PRESSED : (isHovered ? colors::PRIMARY_HOVER : colors::PRIMARY);
        break;
    case ButtonKind::Flat:
        fill = isHeld ? colors::BUTTON_PRESSED : (isHovered ? colors::BUTTON : Color{0, 0, 0, 0});
        textColor = isHovered ? colors::TEXT : colors::TEXT_DIM;
        break;
    case ButtonKind::Normal:
        fill = isHeld ? colors::BUTTON_PRESSED : (isHovered ? colors::BUTTON_HOVER : colors::BUTTON);
        break;
    }
    if (fill.A > 0)
    {
        DrawRoundedRect(rect, std::min(rect.Height() * 0.3f, 18.0f), fill);
    }
    TextOptions options;
    options.Size = textSize;
    options.Tint = textColor;
    options.Align = hyoshi::renderer::TextAlign::Center;
    options.IsBold = true;
    options.MaxWidth = rect.Width() - 16.0f;
    DrawText(label, rect.Center(), options, TEXT_LAYER);
    return isClicked;
}

bool Ui::Toggle(const Rect& rect, std::string_view label, bool& value)
{
    bool isHovered = false;
    bool isHeld = false;
    const bool isClicked = Interact(GetId(label, rect), rect, isHovered, isHeld);
    if (isClicked)
    {
        value = !value;
    }

    if (isHovered)
    {
        DrawRoundedRect(rect, 16.0f, colors::PANEL_RAISED);
    }
    TextOptions options;
    options.Size = 32.0f;
    options.MaxWidth = rect.Width() - 140.0f;
    DrawText(label, {rect.Min.x + 20.0f, rect.Center().y}, options, TEXT_LAYER);

    const glm::vec2 trackSize{84.0f, 44.0f};
    const Rect track =
        Rect::FromSize({rect.Max.x - 20.0f - trackSize.x, rect.Center().y - (trackSize.y * 0.5f)}, trackSize);
    DrawRoundedRect(track, trackSize.y * 0.5f, value ? colors::PRIMARY : colors::BUTTON_PRESSED, 1);
    const float knobSize = trackSize.y - 10.0f;
    const float knobX = value ? track.Max.x - 5.0f - knobSize : track.Min.x + 5.0f;
    DrawRoundedRect(Rect::FromSize({knobX, track.Min.y + 5.0f}, glm::vec2(knobSize)), knobSize * 0.5f, colors::TEXT,
                    TEXT_LAYER);
    return isClicked;
}

bool Ui::Slider(const Rect& rect, std::string_view label, float& value, float min, float max, float step,
                std::string_view valueText)
{
    const float before = value;
    Rect area = rect;
    const Rect header = area.TakeTop(rect.Height() * 0.42f);

    TextOptions labelOptions;
    labelOptions.Size = 30.0f;
    labelOptions.MaxWidth = header.Width() * 0.6f;
    DrawText(label, {header.Min.x + 20.0f, header.Center().y}, labelOptions, TEXT_LAYER);
    TextOptions valueOptions;
    valueOptions.Size = 30.0f;
    valueOptions.Tint = colors::ACCENT;
    valueOptions.Align = hyoshi::renderer::TextAlign::Right;
    valueOptions.IsBold = true;
    DrawText(valueText, {header.Max.x - 20.0f, header.Center().y}, valueOptions, TEXT_LAYER);

    const float buttonSize = std::min(area.Height(), 60.0f);
    const Rect minus = area.TakeLeft(buttonSize + 12.0f).Inset(6.0f, (area.Height() - buttonSize) * 0.5f);
    const Rect plus = area.TakeRight(buttonSize + 12.0f).Inset(6.0f, (area.Height() - buttonSize) * 0.5f);
    if (Button(minus, "-", ButtonKind::Normal, 36.0f))
    {
        value -= step;
    }
    if (Button(plus, "+", ButtonKind::Normal, 36.0f))
    {
        value += step;
    }

    const Rect track = area.Inset(22.0f, 0.0f);
    const uint64_t id = GetId(label, track);
    if (IsHovered(track) && isPointerPressed)
    {
        activeId = id;
    }
    if (activeId == id && isPointerDown && track.Width() > 0.0f)
    {
        const float t = std::clamp((pointer.x - track.Min.x) / track.Width(), 0.0f, 1.0f);
        value = min + (t * (max - min));
    }
    if (step > 0.0f)
    {
        value = min + (std::round((value - min) / step) * step);
    }
    value = std::clamp(value, min, max);

    const float t = max > min ? (value - min) / (max - min) : 0.0f;
    const float knobX = track.Min.x + (t * track.Width());
    const float centerY = track.Center().y;
    DrawRoundedRect({{track.Min.x, centerY - 5.0f}, {track.Max.x, centerY + 5.0f}}, 5.0f, colors::BUTTON_PRESSED);
    DrawRoundedRect({{track.Min.x, centerY - 5.0f}, {knobX, centerY + 5.0f}}, 5.0f, colors::PRIMARY, 1);
    const float knob = activeId == id ? 40.0f : 34.0f;
    DrawRoundedRect(Rect::FromSize({knobX - (knob * 0.5f), centerY - (knob * 0.5f)}, glm::vec2(knob)), knob * 0.5f,
                    colors::TEXT, TEXT_LAYER);
    return value != before;
}

bool Ui::Choice(const Rect& rect, std::string_view label, std::span<const std::string_view> options, size_t& selected)
{
    TextOptions labelOptions;
    labelOptions.Size = 32.0f;
    labelOptions.MaxWidth = rect.Width() * 0.4f;
    DrawText(label, {rect.Min.x + 20.0f, rect.Center().y}, labelOptions, TEXT_LAYER);
    if (options.empty())
    {
        return false;
    }

    // The options share the right part of the row as one segmented control.
    Rect row = rect.Inset(0.0f, 10.0f);
    row.Min.x = rect.Min.x + (rect.Width() * 0.42f);
    row.Max.x -= 12.0f;
    DrawRoundedRect(row, row.Height() * 0.5f, colors::BUTTON_PRESSED);
    const float width = row.Width() / static_cast<float>(options.size());
    const size_t before = selected;
    for (size_t i = 0; i < options.size(); ++i)
    {
        const Rect segment =
            Rect::FromSize({row.Min.x + (width * static_cast<float>(i)), row.Min.y}, {width, row.Height()}).Inset(4.0f);
        bool isHovered = false;
        bool isHeld = false;
        if (Interact(GetId(options[i], segment), segment, isHovered, isHeld))
        {
            selected = i;
        }
        const bool isSelected = i == selected;
        if (isSelected || isHovered)
        {
            DrawRoundedRect(segment, segment.Height() * 0.5f, isSelected ? colors::PRIMARY : colors::BUTTON_HOVER, 1);
        }
        TextOptions optionText;
        optionText.Size = 28.0f;
        optionText.IsBold = true;
        optionText.Align = hyoshi::renderer::TextAlign::Center;
        optionText.Tint = isSelected ? colors::TEXT : colors::TEXT_DIM;
        optionText.MaxWidth = segment.Width() - 12.0f;
        DrawText(options[i], segment.Center(), optionText, TEXT_LAYER);
    }
    return selected != before;
}

void Ui::Scroll(ScrollState& state, const Rect& viewport, float contentHeight)
{
    const float maxOffset = std::max(0.0f, contentHeight - viewport.Height());
    const bool isInside = isInputEnabled && viewport.Contains(pointer);

    if (isInside && wheel != 0.0f)
    {
        state.Target -= wheel * WHEEL_STEP;
        state.Velocity = 0.0f;
    }
    if (isInside && isPointerPressed)
    {
        state.IsPressed = true;
        state.IsDragging = false;
        state.DragStartOffset = state.Offset;
        state.DragStartY = pointer.y;
        state.Velocity = 0.0f;
    }
    if (state.IsPressed && (isPointerDown || isPointerReleased) && isDrag)
    {
        state.IsDragging = true;
        const float offset = std::clamp(state.DragStartOffset - (pointer.y - state.DragStartY), 0.0f, maxOffset);
        if (deltaSeconds > 0.0f)
        {
            const float velocity = (offset - state.Offset) / deltaSeconds;
            state.Velocity += (velocity - state.Velocity) * 0.4f;
        }
        state.Offset = offset;
        state.Target = offset;
    }
    if (state.IsPressed && !isPointerDown)
    {
        if (!state.IsDragging || !isTouch)
        {
            state.Velocity = 0.0f;
        }
        state.IsPressed = false;
        state.IsDragging = false;
    }

    if (!state.IsDragging)
    {
        if (std::abs(state.Velocity) > 5.0f)
        {
            state.Target += state.Velocity * deltaSeconds;
            state.Velocity *= std::exp(-FLICK_DECAY * deltaSeconds);
        }
        else
        {
            state.Velocity = 0.0f;
        }
        state.Target = std::clamp(state.Target, 0.0f, maxOffset);
        state.Offset += (state.Target - state.Offset) * (1.0f - std::exp(-SCROLL_SMOOTHING * deltaSeconds));
    }
    state.Offset = std::clamp(state.Offset, 0.0f, maxOffset);
}

} // namespace hyoshi::ui
