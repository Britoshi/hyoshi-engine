#include "renderer/Camera3D.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include <optional>

namespace
{

using hyoshi::renderer::Camera3D;

constexpr glm::vec2 CANVAS{1920.0f, 1080.0f};

Camera3D MakeCamera()
{
    Camera3D camera;
    camera.Position = {1.0f, 2.0f, 4.0f};
    camera.Target = {0.0f, 1.0f, 0.0f};
    camera.FieldOfViewDegrees = 50.0f;
    return camera;
}

} // namespace

TEST_CASE("Camera3D projects what it looks at to the canvas's middle, and up to its top")
{
    const Camera3D camera = MakeCamera();
    const std::optional<glm::vec2> middle = camera.ProjectToCanvas(camera.Target, CANVAS);
    REQUIRE(middle);
    CHECK(middle->x == doctest::Approx(960.0f).epsilon(0.001));
    CHECK(middle->y == doctest::Approx(540.0f).epsilon(0.001));
    const std::optional<glm::vec2> above = camera.ProjectToCanvas(camera.Target + glm::vec3(0.0f, 0.5f, 0.0f), CANVAS);
    REQUIRE(above);
    CHECK(above->y < 540.0f);
    // Behind the camera: nowhere on the canvas.
    CHECK_FALSE(camera.ProjectToCanvas(camera.Position + (camera.Position - camera.Target), CANVAS));
}

TEST_CASE("Camera3D's ray through a point on the canvas passes through what lands there")
{
    const Camera3D camera = MakeCamera();
    for (const glm::vec3 point : {glm::vec3(0.3f, 1.4f, -0.5f), glm::vec3(-1.2f, 0.2f, 0.4f), glm::vec3(0.0f)})
    {
        const std::optional<glm::vec2> onCanvas = camera.ProjectToCanvas(point, CANVAS);
        REQUIRE(onCanvas);
        const glm::vec3 ray = camera.GetRayThrough(*onCanvas, CANVAS);
        CHECK(glm::length(ray) == doctest::Approx(1.0f));
        const glm::vec3 toPoint = glm::normalize(point - camera.Position);
        CHECK(glm::dot(ray, toPoint) == doctest::Approx(1.0f).epsilon(0.0001));
    }
    // The middle of the canvas: straight ahead.
    const glm::vec3 ahead = camera.GetRayThrough(CANVAS * 0.5f, CANVAS);
    CHECK(glm::dot(ahead, glm::normalize(camera.Target - camera.Position)) == doctest::Approx(1.0f));
}
