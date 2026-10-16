#include "renderer/Camera3D.h"

#include "renderer/Camera2D.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace hyoshi::renderer
{
namespace
{

float GetAspect(glm::vec2 canvas)
{
    return canvas.y > 0.0f ? canvas.x / canvas.y : 1.0f;
}

} // namespace

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

std::optional<glm::vec2> Camera3D::ProjectToCanvas(glm::vec3 point, glm::vec2 canvas) const
{
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(FieldOfViewDegrees), GetAspect(canvas), Near, Far);
    const glm::vec4 clip = projection * GetView() * glm::vec4(point, 1.0f);
    if (clip.w <= 0.0f)
    {
        return std::nullopt;
    }
    const glm::vec2 device = glm::vec2(clip) / clip.w;
    return glm::vec2((device.x + 1.0f) * 0.5f * canvas.x, (1.0f - device.y) * 0.5f * canvas.y);
}

glm::vec3 Camera3D::GetRayThrough(glm::vec2 canvasPoint, glm::vec2 canvas) const
{
    // The point on the canvas as a share of the view's half height and width, up and right.
    const glm::vec2 device{canvas.x > 0.0f ? ((canvasPoint.x / canvas.x) * 2.0f) - 1.0f : 0.0f,
                           canvas.y > 0.0f ? 1.0f - ((canvasPoint.y / canvas.y) * 2.0f) : 0.0f};
    const float halfHeight = std::tan(glm::radians(FieldOfViewDegrees) * 0.5f);
    const glm::mat4 view = GetView();
    const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
    const glm::vec3 up{view[0][1], view[1][1], view[2][1]};
    const glm::vec3 forward{-view[0][2], -view[1][2], -view[2][2]};
    return glm::normalize(forward + (right * (device.x * halfHeight * GetAspect(canvas))) +
                          (up * (device.y * halfHeight)));
}

} // namespace hyoshi::renderer
