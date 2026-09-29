#pragma once

#include "input/InputEvent.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::input
{

// The part of a trackpad that maps onto the whole window, as tablet drivers map an area of the
// tablet onto the screen. Positions are normalized 0..1 from the top left, on the pad (Trackpad
// events) and in the window. The area has the window's shape.
//
// The pad's real size isn't known (SDL doesn't report it), so it's taken to have the window's
// shape too: turning the area happens in units as wide as they are tall in the window, and a pad
// of another shape stretches it.
struct TrackpadArea
{
    // Width and height as a fraction of the pad's. Smaller is faster: a short move goes far.
    float Size = 1.0f;
    // Where the area's center sits on the pad.
    glm::vec2 Center{0.5f, 0.5f};
    // Clockwise, as the area sits on the pad: its top edge is what the window calls up.
    float RotationDegrees = 0.0f;
};

// Where a finger at padPosition aims in the window, clamped to it. windowAspect is the window's
// width over its height.
glm::vec2 MapTrackpadToWindow(const TrackpadArea& area, glm::vec2 padPosition, float windowAspect);

// The inverse, unclamped: where on the pad a window position comes from, for drawing the area.
glm::vec2 MapWindowToTrackpad(const TrackpadArea& area, glm::vec2 windowPosition, float windowAspect);

// The fingers on a trackpad in the order they landed, from Trackpad events. The newest aims.
class TrackpadFingers
{
public:
    struct Finger
    {
        uint64_t Id = 0;
        // On the pad, 0..1.
        glm::vec2 Position{0.0f};
    };

    // Takes Trackpad events and ignores the rest. Returns true when the newest finger's position
    // changed: one landed, the newest moved, or it lifted and an older one aims again. A move from
    // a finger it hasn't seen land (it landed before the events came here) adds it as the newest.
    bool Apply(const InputEvent& event);

    const std::vector<Finger>& Get() const
    {
        return fingers;
    }

    std::optional<glm::vec2> GetNewest() const;

    void Clear()
    {
        fingers.clear();
    }

private:
    std::vector<Finger> fingers;
};

} // namespace hyoshi::input
