#pragma once

#include "rhi/RhiTypes.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

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
};

} // namespace hyoshi::renderer
