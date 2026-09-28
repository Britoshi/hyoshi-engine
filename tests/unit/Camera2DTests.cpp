#include "renderer/Camera2D.h"

#include <doctest/doctest.h>
#include <glm/common.hpp>

#include <algorithm>
#include <array>

namespace
{

using hyoshi::rhi::Extent;
using hyoshi::rhi::Rect;
using hyoshi::rhi::SurfaceTransform;

constexpr std::array<SurfaceTransform, 4> ALL_TRANSFORMS{SurfaceTransform::Identity, SurfaceTransform::Rotate90,
                                                         SurfaceTransform::Rotate180, SurfaceTransform::Rotate270};

bool IsQuarterTurn(SurfaceTransform transform)
{
    return transform == SurfaceTransform::Rotate90 || transform == SurfaceTransform::Rotate270;
}

// What the 2D shaders do with a position, followed by the viewport transform to swapchain pixels.
glm::vec2 ToSwapchainPixel(const hyoshi::renderer::ClipTransform& clip, glm::vec2 position, Extent swapchain)
{
    const glm::vec2 p = (position * clip.Scale) + clip.Offset;
    const glm::vec2 r = clip.Rotation;
    const glm::vec2 clipPosition{(p.x * r.x) - (p.y * r.y), (p.x * r.y) + (p.y * r.x)};
    return {(clipPosition.x + 1.0f) * 0.5f * static_cast<float>(swapchain.Width),
            (clipPosition.y + 1.0f) * 0.5f * static_cast<float>(swapchain.Height)};
}

} // namespace

// Which way a quarter turn goes can only be checked on a rotated device, but the scissor mapping
// must agree with the vertex mapping whatever the direction, or clipped debug UI tears.
TEST_CASE("ToSwapchainRect covers the pixels the clip transform draws a rectangle on")
{
    const Extent logical{800, 600};
    const Rect rect{100, 50, 200, 120};

    for (const SurfaceTransform transform : ALL_TRANSFORMS)
    {
        CAPTURE(static_cast<int>(transform));
        const Extent swapchain = IsQuarterTurn(transform) ? Extent{logical.Height, logical.Width} : logical;

        const Extent roundTrip = hyoshi::renderer::GetLogicalExtent(swapchain, transform);
        CHECK(roundTrip.Width == logical.Width);
        CHECK(roundTrip.Height == logical.Height);

        const hyoshi::renderer::ClipTransform clip = hyoshi::renderer::MakePixelClipTransform(
            {static_cast<float>(logical.Width), static_cast<float>(logical.Height)}, transform);

        const auto left = static_cast<float>(rect.X);
        const auto top = static_cast<float>(rect.Y);
        const float right = left + static_cast<float>(rect.Width);
        const float bottom = top + static_cast<float>(rect.Height);
        glm::vec2 minCorner{1.0e9f};
        glm::vec2 maxCorner{-1.0e9f};
        for (const glm::vec2 corner :
             {glm::vec2{left, top}, glm::vec2{right, top}, glm::vec2{left, bottom}, glm::vec2{right, bottom}})
        {
            const glm::vec2 pixel = ToSwapchainPixel(clip, corner, swapchain);
            minCorner = glm::min(minCorner, pixel);
            maxCorner = glm::max(maxCorner, pixel);
        }

        const Rect scissor = hyoshi::renderer::ToSwapchainRect(rect, logical, transform);
        CHECK(static_cast<float>(scissor.X) == doctest::Approx(minCorner.x));
        CHECK(static_cast<float>(scissor.Y) == doctest::Approx(minCorner.y));
        CHECK(static_cast<float>(scissor.Width) == doctest::Approx(maxCorner.x - minCorner.x));
        CHECK(static_cast<float>(scissor.Height) == doctest::Approx(maxCorner.y - minCorner.y));
    }
}

TEST_CASE("Camera2D keeps the virtual height and fills the swapchain")
{
    for (const SurfaceTransform transform : ALL_TRANSFORMS)
    {
        CAPTURE(static_cast<int>(transform));
        const Extent swapchain = IsQuarterTurn(transform) ? Extent{1440, 2560} : Extent{2560, 1440};

        hyoshi::renderer::Camera2D camera(1080.0f);
        camera.SetTarget(swapchain, transform);

        const glm::vec2 size = camera.GetVirtualSize();
        CHECK(size.x == doctest::Approx(1920.0f));
        CHECK(size.y == doctest::Approx(1080.0f));

        // The virtual canvas maps onto exactly the whole swapchain.
        const glm::vec2 a = ToSwapchainPixel(camera.GetClipTransform(), {0.0f, 0.0f}, swapchain);
        const glm::vec2 b = ToSwapchainPixel(camera.GetClipTransform(), size, swapchain);
        CHECK(std::min(a.x, b.x) == doctest::Approx(0.0f));
        CHECK(std::min(a.y, b.y) == doctest::Approx(0.0f));
        CHECK(std::max(a.x, b.x) == doctest::Approx(static_cast<float>(swapchain.Width)));
        CHECK(std::max(a.y, b.y) == doctest::Approx(static_cast<float>(swapchain.Height)));
    }
}

TEST_CASE("Camera2D fixes the shorter side, so layouts can tell the orientation")
{
    hyoshi::renderer::Camera2D camera;

    camera.SetTarget({1280, 720}, SurfaceTransform::Identity);
    CHECK(camera.GetVirtualSize().x == doctest::Approx(1920.0f));
    CHECK(camera.GetVirtualSize().y == doctest::Approx(1080.0f));

    camera.SetTarget({720, 1280}, SurfaceTransform::Identity);
    CHECK(camera.GetVirtualSize().x == doctest::Approx(1080.0f));
    CHECK(camera.GetVirtualSize().y == doctest::Approx(1920.0f));

    // A portrait phone whose swapchain is landscape and pre-rotated.
    camera.SetTarget({2340, 1080}, SurfaceTransform::Rotate90);
    CHECK(camera.GetVirtualSize().x == doctest::Approx(1080.0f));
    CHECK(camera.GetVirtualSize().y == doctest::Approx(2340.0f));

    // A zero-sized swapchain (minimized) keeps the last size.
    camera.SetTarget({0, 0}, SurfaceTransform::Identity);
    CHECK(camera.GetVirtualSize().x == doctest::Approx(1080.0f));
}
