#include "input/Trackpad.h"

#include <doctest/doctest.h>

#include <cstdint>

using hyoshi::input::InputEvent;
using hyoshi::input::InputEventType;
using hyoshi::input::TrackpadArea;
using hyoshi::input::TrackpadFingers;

namespace
{

constexpr float ASPECT = 16.0f / 10.0f;

void CheckNear(glm::vec2 actual, glm::vec2 expected)
{
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-5));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-5));
}

InputEvent Finger(InputEventType type, uint64_t id, glm::vec2 position)
{
    InputEvent event;
    event.Type = type;
    event.FingerId = id;
    event.X = position.x;
    event.Y = position.y;
    return event;
}

} // namespace

TEST_CASE("Trackpad area: the whole pad maps onto the whole window")
{
    const TrackpadArea area;
    CheckNear(MapTrackpadToWindow(area, {0.0f, 0.0f}, ASPECT), {0.0f, 0.0f});
    CheckNear(MapTrackpadToWindow(area, {0.25f, 0.75f}, ASPECT), {0.25f, 0.75f});
    CheckNear(MapTrackpadToWindow(area, {1.0f, 1.0f}, ASPECT), {1.0f, 1.0f});
}

TEST_CASE("Trackpad area: a smaller area goes further, and stops at the window's edges")
{
    TrackpadArea area;
    area.Size = 0.5f;
    area.Center = {0.75f, 0.5f};
    // The area covers 0.5 to 1 across and 0.25 to 0.75 down.
    CheckNear(MapTrackpadToWindow(area, {0.75f, 0.5f}, ASPECT), {0.5f, 0.5f});
    CheckNear(MapTrackpadToWindow(area, {0.5f, 0.25f}, ASPECT), {0.0f, 0.0f});
    CheckNear(MapTrackpadToWindow(area, {0.875f, 0.625f}, ASPECT), {0.75f, 0.75f});
    // Outside it, the aim stays on the window's edge.
    CheckNear(MapTrackpadToWindow(area, {0.1f, 0.9f}, ASPECT), {0.0f, 1.0f});
}

TEST_CASE("Trackpad area: turned clockwise, its top edge is up")
{
    TrackpadArea area;
    area.Size = 0.5f;
    area.RotationDegrees = 90.0f;
    // A quarter turn clockwise puts the area's top edge on the pad's right: moving the finger
    // right on the pad moves the aim up, and down on the pad moves it right.
    const glm::vec2 center = MapTrackpadToWindow(area, {0.5f, 0.5f}, ASPECT);
    CheckNear(center, {0.5f, 0.5f});
    const glm::vec2 right = MapTrackpadToWindow(area, {0.55f, 0.5f}, ASPECT);
    CHECK(right.x == doctest::Approx(0.5f));
    CHECK(right.y < 0.5f);
    const glm::vec2 down = MapTrackpadToWindow(area, {0.5f, 0.55f}, ASPECT);
    CHECK(down.x > 0.5f);
    CHECK(down.y == doctest::Approx(0.5f));
    // In square units: 0.05 of the pad's height is 0.05 / (0.5 * ASPECT) of the window's width.
    CHECK(down.x == doctest::Approx(0.5f + (0.05f / (0.5f * ASPECT))));
}

TEST_CASE("Trackpad area: mapping back to the pad undoes it")
{
    TrackpadArea area;
    area.Size = 0.6f;
    area.Center = {0.4f, 0.55f};
    area.RotationDegrees = -30.0f;
    for (const glm::vec2 window : {glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 0.0f}, glm::vec2{0.3f, 0.8f}})
    {
        CheckNear(MapTrackpadToWindow(area, MapWindowToTrackpad(area, window, ASPECT), ASPECT), window);
    }
    CheckNear(MapWindowToTrackpad(area, {0.5f, 0.5f}, ASPECT), area.Center);
}

TEST_CASE("Trackpad fingers: the newest aims, and an older one takes over when it lifts")
{
    TrackpadFingers fingers;
    CHECK_FALSE(fingers.GetNewest());

    CHECK(fingers.Apply(Finger(InputEventType::TrackpadDown, 1, {0.1f, 0.1f})));
    CHECK(fingers.Apply(Finger(InputEventType::TrackpadDown, 2, {0.9f, 0.9f})));
    CheckNear(*fingers.GetNewest(), {0.9f, 0.9f});
    // An older finger moving doesn't move the aim.
    CHECK_FALSE(fingers.Apply(Finger(InputEventType::TrackpadMove, 1, {0.2f, 0.2f})));
    CHECK(fingers.Apply(Finger(InputEventType::TrackpadMove, 2, {0.8f, 0.8f})));
    // The newest lifts: the older one aims from where it is now.
    CHECK(fingers.Apply(Finger(InputEventType::TrackpadUp, 2, {0.8f, 0.8f})));
    CheckNear(*fingers.GetNewest(), {0.2f, 0.2f});
    // A move from a finger that landed unseen joins as the newest.
    CHECK(fingers.Apply(Finger(InputEventType::TrackpadMove, 3, {0.5f, 0.4f})));
    CHECK(fingers.Get().size() == 2);
    // An older finger lifting, an unknown one lifting, and other events leave the aim alone.
    CHECK_FALSE(fingers.Apply(Finger(InputEventType::TrackpadUp, 1, {0.2f, 0.2f})));
    CHECK_FALSE(fingers.Apply(Finger(InputEventType::TrackpadUp, 7, {0.0f, 0.0f})));
    CHECK_FALSE(fingers.Apply(Finger(InputEventType::TouchDown, 8, {0.0f, 0.0f})));
    CheckNear(*fingers.GetNewest(), {0.5f, 0.4f});
    // The last one lifting leaves nothing to aim with.
    CHECK_FALSE(fingers.Apply(Finger(InputEventType::TrackpadUp, 3, {0.5f, 0.4f})));
    CHECK_FALSE(fingers.GetNewest());
}
