#include "renderer/Camera3D.h"

#include "renderer/Camera2D.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace hyoshi::renderer
{

glm::mat4 Camera3D::GetView() const
{
    return glm::lookAtRH(Position, Target, Up);
}

glm::mat4 Camera3D::GetClipFromWorld(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform) const
{
    const rhi::Extent seen = GetLogicalExtent(swapchainExtent, transform);
    const float aspect = seen.Height > 0 ? static_cast<float>(seen.Width) / static_cast<float>(seen.Height) : 1.0f;
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(FieldOfViewDegrees), aspect, Near, Far);
    // Vulkan's clip space points down. The picture comes out upright, so a triangle seen
    // counter-clockwise still is.
    projection[1][1] = -projection[1][1];

    // The swapchain's pre-rotation, turning clip space as Camera2D's ClipTransform does.
    const glm::vec2 rotation = MakePixelClipTransform({1.0f, 1.0f}, transform).Rotation;
    glm::mat4 turn(1.0f);
    turn[0][0] = rotation.x;
    turn[0][1] = rotation.y;
    turn[1][0] = -rotation.y;
    turn[1][1] = rotation.x;
    return turn * projection * GetView();
}

} // namespace hyoshi::renderer
