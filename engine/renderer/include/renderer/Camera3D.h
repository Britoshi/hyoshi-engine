#pragma once

#include "rhi/RhiTypes.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace hyoshi::renderer
{

// A perspective camera for 3D (decision 0003). The world is right-handed with y up; the camera at
// Position looks at Target.
struct Camera3D
{
    glm::vec3 Position{0.0f, 1.0f, 3.0f};
    glm::vec3 Target{0.0f};
    glm::vec3 Up{0.0f, 1.0f, 0.0f};
    // Top to bottom of the view, in degrees.
    float FieldOfViewDegrees = 45.0f;
    // What's nearer or farther than these isn't drawn.
    float Near = 0.05f;
    float Far = 100.0f;

    glm::mat4 GetView() const;
    // World space to clip space as the swapchain takes it: Vulkan's (y down, depth 0 to 1), at the
    // aspect of the swapchain as it's seen, and turned by its pre-rotation, as Camera2D's is.
    glm::mat4 GetClipFromWorld(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform) const;

    // A canvas is the screen as it's seen (Camera2D's: `canvas` across and down, from the top left,
    // y down), whatever the swapchain's pre-rotation. Where `point` lands on one, or none if it's
    // behind the camera.
    std::optional<glm::vec2> ProjectToCanvas(glm::vec3 point, glm::vec2 canvas) const;
    // The direction (unit length) from Position through a point on a canvas: for picking what's
    // under the pointer.
    glm::vec3 GetRayThrough(glm::vec2 canvasPoint, glm::vec2 canvas) const;
};

} // namespace hyoshi::renderer
