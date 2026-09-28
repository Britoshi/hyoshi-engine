#include "renderer/Camera2D.h"

namespace hyoshi::renderer
{

namespace
{

// Rotating clip space by the surface transform renders the frame "pre-rotated", following the
// Android pre-rotation guide. Not yet verified on a rotated Android device.
glm::vec2 RotationFor(rhi::SurfaceTransform transform)
{
    switch (transform)
    {
    case rhi::SurfaceTransform::Rotate90:
        return {0.0f, 1.0f};
    case rhi::SurfaceTransform::Rotate180:
        return {-1.0f, 0.0f};
    case rhi::SurfaceTransform::Rotate270:
        return {0.0f, -1.0f};
    case rhi::SurfaceTransform::Identity:
        break;
    }
    return {1.0f, 0.0f};
}

bool IsQuarterTurn(rhi::SurfaceTransform transform)
{
    return transform == rhi::SurfaceTransform::Rotate90 || transform == rhi::SurfaceTransform::Rotate270;
}

} // namespace

Camera2D::Camera2D(float shortSideLength) : shortSide(shortSideLength)
{
}

void Camera2D::SetTarget(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform)
{
    const rhi::Extent logical = GetLogicalExtent(swapchainExtent, transform);
    if (logical.Width == 0 || logical.Height == 0)
    {
        return;
    }

    const auto width = static_cast<float>(logical.Width);
    const auto height = static_cast<float>(logical.Height);
    virtualSize = width >= height ? glm::vec2{shortSide * width / height, shortSide}
                                  : glm::vec2{shortSide, shortSide * height / width};
    clipTransform = MakePixelClipTransform(virtualSize, transform);
}

ClipTransform MakePixelClipTransform(glm::vec2 logicalSize, rhi::SurfaceTransform transform)
{
    // Vulkan clip space has y pointing down, matching a top-left origin.
    ClipTransform result;
    result.Scale = {2.0f / logicalSize.x, 2.0f / logicalSize.y};
    result.Offset = {-1.0f, -1.0f};
    result.Rotation = RotationFor(transform);
    return result;
}

rhi::Extent GetLogicalExtent(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform)
{
    if (IsQuarterTurn(transform))
    {
        return {swapchainExtent.Height, swapchainExtent.Width};
    }
    return swapchainExtent;
}

rhi::Rect ToSwapchainRect(const rhi::Rect& logicalRect, rhi::Extent logicalExtent, rhi::SurfaceTransform transform)
{
    const int32_t width = static_cast<int32_t>(logicalExtent.Width);
    const int32_t height = static_cast<int32_t>(logicalExtent.Height);
    const int32_t x0 = logicalRect.X;
    const int32_t y0 = logicalRect.Y;
    const int32_t x1 = logicalRect.X + static_cast<int32_t>(logicalRect.Width);
    const int32_t y1 = logicalRect.Y + static_cast<int32_t>(logicalRect.Height);

    // Where the rotated clip transform sends logical pixel (x, y), derived from RotationFor().
    switch (transform)
    {
    case rhi::SurfaceTransform::Rotate90: // (x, y) -> (height - y, x)
        return {height - y1, x0, logicalRect.Height, logicalRect.Width};
    case rhi::SurfaceTransform::Rotate180: // (x, y) -> (width - x, height - y)
        return {width - x1, height - y1, logicalRect.Width, logicalRect.Height};
    case rhi::SurfaceTransform::Rotate270: // (x, y) -> (y, width - x)
        return {y0, width - x1, logicalRect.Height, logicalRect.Width};
    case rhi::SurfaceTransform::Identity:
        break;
    }
    return logicalRect;
}

} // namespace hyoshi::renderer
