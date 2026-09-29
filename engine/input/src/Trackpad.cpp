#include "input/Trackpad.h"

#include <glm/common.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace hyoshi::input
{

namespace
{

// Turns a vector clockwise on screen (y points down).
glm::vec2 TurnClockwise(glm::vec2 vector, float degrees)
{
    const float radians = glm::radians(degrees);
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return {(cosine * vector.x) - (sine * vector.y), (sine * vector.x) + (cosine * vector.y)};
}

} // namespace

glm::vec2 MapTrackpadToWindow(const TrackpadArea& area, glm::vec2 padPosition, float windowAspect)
{
    // In square units (x times the aspect), where turning is a turn and not a shear.
    const glm::vec2 units{windowAspect, 1.0f};
    const glm::vec2 fromCenter = (padPosition - area.Center) * units;
    // Undo the area's turn: along its top edge is to the right in the window.
    const glm::vec2 inArea = TurnClockwise(fromCenter, -area.RotationDegrees) / (area.Size * units);
    return glm::clamp(inArea + 0.5f, 0.0f, 1.0f);
}

glm::vec2 MapWindowToTrackpad(const TrackpadArea& area, glm::vec2 windowPosition, float windowAspect)
{
    const glm::vec2 units{windowAspect, 1.0f};
    const glm::vec2 inArea = (windowPosition - 0.5f) * (area.Size * units);
    return area.Center + (TurnClockwise(inArea, area.RotationDegrees) / units);
}

bool TrackpadFingers::Apply(const InputEvent& event)
{
    const auto finger = std::find_if(fingers.begin(), fingers.end(),
                                     [&event](const Finger& touch) { return touch.Id == event.FingerId; });
    const glm::vec2 position{event.X, event.Y};
    switch (event.Type)
    {
    case InputEventType::TrackpadDown:
    case InputEventType::TrackpadMove:
        if (finger == fingers.end())
        {
            fingers.push_back({event.FingerId, position});
            return true;
        }
        finger->Position = position;
        return finger == fingers.end() - 1;
    case InputEventType::TrackpadUp:
    {
        if (finger == fingers.end())
        {
            return false;
        }
        const bool wasNewest = finger == fingers.end() - 1;
        fingers.erase(finger);
        return wasNewest && !fingers.empty();
    }
    default:
        return false;
    }
}

std::optional<glm::vec2> TrackpadFingers::GetNewest() const
{
    if (fingers.empty())
    {
        return std::nullopt;
    }
    return fingers.back().Position;
}

} // namespace hyoshi::input
