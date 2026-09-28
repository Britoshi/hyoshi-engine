#pragma once

#include "rhi/RhiTypes.h"

#include <glm/vec2.hpp>

namespace hyoshi::renderer
{

// Maps virtual 2D coordinates to clip space: clip = rotate(position * Scale + Offset, Rotation).
// Pushed as-is to 2D shaders, so the layout must match their DrawConstants.
struct ClipTransform
{
    glm::vec2 Scale{1.0f};
    glm::vec2 Offset{0.0f};
    // cos and sin of the swapchain pre-rotation.
    glm::vec2 Rotation{1.0f, 0.0f};
};

// Orthographic camera over a virtual canvas (DESIGN.md section 10.4). The shorter side is fixed
// (1080 by default) and the longer one follows the display's aspect ratio: 1920 x 1080 in 16:9
// landscape, 1080 x 1920 in portrait. Layouts compare the two to adapt to the orientation. Origin
// top-left, y down, in virtual units. Applies the swapchain pre-rotation.
class Camera2D
{
public:
    explicit Camera2D(float shortSide = 1080.0f);

    // Call whenever the swapchain changes.
    void SetTarget(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform);

    glm::vec2 GetVirtualSize() const
    {
        return virtualSize;
    }

    const ClipTransform& GetClipTransform() const
    {
        return clipTransform;
    }

    // Converts a window position normalized to 0..1 (as touch and pointer input report it) into
    // virtual units.
    glm::vec2 NormalizedToVirtual(glm::vec2 normalized) const
    {
        return normalized * virtualSize;
    }

private:
    float shortSide;
    glm::vec2 virtualSize{0.0f};
    ClipTransform clipTransform;
};

// The clip transform for drawing in unrotated ("logical") pixel coordinates, e.g. for debug UI.
ClipTransform MakePixelClipTransform(glm::vec2 logicalSize, rhi::SurfaceTransform transform);

// The swapchain's size as the user sees it, with width and height swapped back for 90/270 rotation.
rhi::Extent GetLogicalExtent(rhi::Extent swapchainExtent, rhi::SurfaceTransform transform);

// Maps a scissor rectangle in logical pixels to swapchain pixels, consistent with the rotation
// MakePixelClipTransform applies.
rhi::Rect ToSwapchainRect(const rhi::Rect& logicalRect, rhi::Extent logicalExtent, rhi::SurfaceTransform transform);

} // namespace hyoshi::renderer
