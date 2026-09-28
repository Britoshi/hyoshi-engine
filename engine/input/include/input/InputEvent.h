#pragma once

#include "core/Time.h"

#include <cstdint>

namespace hyoshi::input
{

// Raw input with OS timestamps in the host clock domain (DESIGN.md section 12.1). Modes turn these
// into gameplay actions; nothing here knows about lanes or notes.
enum class InputEventType : uint8_t
{
    KeyDown,
    KeyUp,
    TouchDown,
    TouchMove,
    TouchUp,
    PointerDown,
    PointerUp,
    PointerMove,
    AxisChange,
    // A mouse wheel or trackpad scroll: Value is vertical steps (positive away from the user),
    // X horizontal.
    Wheel,
    // A finger on a trackpad that reports where it touches (Mac trackpads). X and Y are normalized
    // 0..1 across the pad's surface from its top left, not the window. The OS still moves the
    // pointer from the same fingers, so most scenes ignore these.
    TrackpadDown,
    TrackpadMove,
    TrackpadUp
};

struct InputEvent
{
    InputEventType Type = InputEventType::KeyDown;
    // When the OS saw the event, on the same clock as the audio snapshots.
    HostTimeNs HostTime = 0;
    uint32_t DeviceId = 0;
    // Touch and trackpad only.
    uint64_t FingerId = 0;
    // Key: a KeyCode. Pointer: the button (1 left, 2 middle, 3 right). Axis: the axis ID.
    uint32_t Code = 0;
    // Normalized 0..1 across the window (touch, pointer), or across the pad (trackpad).
    float X = 0.0f;
    float Y = 0.0f;
    // Axis value, or touch pressure.
    float Value = 0.0f;
    // A KeyDown from the OS repeating a held key. Menus use these; gameplay must ignore them.
    bool IsRepeat = false;
};

// Physical key positions as USB HID usage IDs (the same values as SDL scancodes), so bindings
// follow the key's place on the keyboard rather than its layout's letter.
namespace keys
{

constexpr uint32_t Letter(char letter)
{
    return 4 + static_cast<uint32_t>(letter - 'A');
}

constexpr uint32_t A = Letter('A');
constexpr uint32_t D = Letter('D');
constexpr uint32_t F = Letter('F');
constexpr uint32_t G = Letter('G');
constexpr uint32_t J = Letter('J');
constexpr uint32_t K = Letter('K');
constexpr uint32_t L = Letter('L');
constexpr uint32_t M = Letter('M');
constexpr uint32_t S = Letter('S');
constexpr uint32_t RETURN = 40;
constexpr uint32_t ESCAPE = 41;
constexpr uint32_t BACKSPACE = 42;
constexpr uint32_t TAB = 43;
constexpr uint32_t SPACE = 44;
constexpr uint32_t MINUS = 45;
constexpr uint32_t EQUALS = 46;
constexpr uint32_t SEMICOLON = 51;
constexpr uint32_t F1 = 58;
constexpr uint32_t F2 = 59;
constexpr uint32_t F5 = 62;
constexpr uint32_t HOME = 74;
constexpr uint32_t PAGE_UP = 75;
constexpr uint32_t END = 77;
constexpr uint32_t PAGE_DOWN = 78;
constexpr uint32_t RIGHT = 79;
constexpr uint32_t LEFT = 80;
constexpr uint32_t DOWN = 81;
constexpr uint32_t UP = 82;
constexpr uint32_t KEYPAD_MINUS = 86;
constexpr uint32_t KEYPAD_PLUS = 87;
constexpr uint32_t KEYPAD_ENTER = 88;
constexpr uint32_t LEFT_CTRL = 224;
constexpr uint32_t LEFT_SHIFT = 225;
constexpr uint32_t RIGHT_CTRL = 228;
constexpr uint32_t RIGHT_SHIFT = 229;

} // namespace keys

} // namespace hyoshi::input
