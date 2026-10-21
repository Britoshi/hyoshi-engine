#include "renderer/Mesh.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace
{

using hyoshi::renderer::MeshData;

float GetArea(const MeshData& mesh)
{
    float area = 0.0f;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
    {
        const glm::vec3 a = mesh.Vertices[mesh.Indices[i]].Position;
        const glm::vec3 b = mesh.Vertices[mesh.Indices[i + 1]].Position;
        const glm::vec3 c = mesh.Vertices[mesh.Indices[i + 2]].Position;
        area += glm::length(glm::cross(b - a, c - a)) * 0.5f;
    }
    return area;
}

// The sum of each triangle's facing (its area along its corners' normals): positive when every
// triangle winds as its normals face.
float GetFacing(const MeshData& mesh)
{
    float facing = 0.0f;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
    {
        const glm::vec3 a = mesh.Vertices[mesh.Indices[i]].Position;
        const glm::vec3 b = mesh.Vertices[mesh.Indices[i + 1]].Position;
        const glm::vec3 c = mesh.Vertices[mesh.Indices[i + 2]].Position;
        facing += glm::dot(glm::cross(b - a, c - a), mesh.Vertices[mesh.Indices[i]].Normal);
    }
    return facing;
}

} // namespace

TEST_CASE("SliceMesh cuts a box into slabs that keep its surface and its winding")
{
    MeshData box;
    hyoshi::renderer::AddBox(box, glm::mat4(1.0f));
    const float area = GetArea(box);
    REQUIRE(area == doctest::Approx(6.0f));

    // Three slabs along y from -0.25 to 0.5: the first takes the bottom half too.
    const std::vector<MeshData> slabs = hyoshi::renderer::SliceMesh(box, {0.0f, 1.0f, 0.0f}, -0.25f, 0.5f, 3);
    REQUIRE(slabs.size() == 3);
    const std::vector<float> lows{-0.5f, 0.0f, 0.25f};
    const std::vector<float> highs{0.0f, 0.25f, 0.5f};
    float total = 0.0f;
    for (size_t slab = 0; slab < slabs.size(); ++slab)
    {
        CHECK(!slabs[slab].Indices.empty());
        for (const hyoshi::renderer::MeshVertex& vertex : slabs[slab].Vertices)
        {
            CHECK(vertex.Position.y >= lows[slab] - 1.0e-5f);
            CHECK(vertex.Position.y <= highs[slab] + 1.0e-5f);
            CHECK(glm::length(vertex.Normal) == doctest::Approx(1.0f));
        }
        CHECK(GetFacing(slabs[slab]) > 0.0f);
        total += GetArea(slabs[slab]);
    }
    CHECK(total == doctest::Approx(area));
    // The bottom slab: the bottom face, and half of each side (4 x 0.5).
    CHECK(GetArea(slabs[0]) == doctest::Approx(3.0f));
}

TEST_CASE("SliceMesh blends the points it adds")
{
    MeshData strip;
    strip.Vertices = {{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, 0xFF000000},
                      {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, 0xFF000000},
                      {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, 0xFFFFFFFF}};
    strip.Indices = {0, 1, 2};
    const std::vector<MeshData> slabs = hyoshi::renderer::SliceMesh(strip, {0.0f, 1.0f, 0.0f}, 0.0f, 1.0f, 2);
    REQUIRE(slabs.size() == 2);
    bool hasHalfway = false;
    for (const hyoshi::renderer::MeshVertex& vertex : slabs[1].Vertices)
    {
        if (vertex.Position.y == doctest::Approx(0.5f) && vertex.Position.x == doctest::Approx(0.0f))
        {
            hasHalfway = true;
            CHECK(vertex.Uv.y == doctest::Approx(0.5f));
            // Red from 0 to 255, halfway: 128 (rounded); alpha stays 255.
            CHECK((vertex.Color & 0xFFU) == 128U);
            CHECK((vertex.Color >> 24) == 0xFFU);
        }
    }
    CHECK(hasHalfway);
    CHECK(GetArea(slabs[0]) + GetArea(slabs[1]) == doctest::Approx(0.5f));
}

TEST_CASE("WithLightUvs splits a vertex only where its corners lie apart in the lightmap")
{
    // Two triangles sharing an edge: its two vertices shared, one in the same place in the lightmap
    // for both triangles and one not.
    MeshData quad;
    quad.Vertices.resize(4);
    for (size_t i = 0; i < 4; ++i)
    {
        quad.Vertices[i].Position = glm::vec3(static_cast<float>(i), 0.0f, 0.0f);
    }
    quad.Indices = {0, 1, 2, 2, 1, 3};
    const std::vector<glm::vec2> corners{{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.5f},
                                         {0.0f, 0.5f}, {0.9f, 0.9f}, {0.5f, 0.5f}};
    const MeshData lit = hyoshi::renderer::WithLightUvs(quad, corners);
    REQUIRE(lit.Indices.size() == 6);
    // Vertex 1 is in two places (0.5, 0) and (0.9, 0.9); vertex 2 in one.
    CHECK(lit.Vertices.size() == 5);
    for (size_t i = 0; i < 6; ++i)
    {
        CHECK(lit.Vertices[lit.Indices[i]].Position == quad.Vertices[quad.Indices[i]].Position);
        CHECK(lit.Vertices[lit.Indices[i]].LightUv == corners[i]);
    }
    CHECK(lit.Indices[2] == lit.Indices[3]);
    CHECK(lit.Indices[1] != lit.Indices[4]);
    // Not one for each index: nothing.
    CHECK(hyoshi::renderer::WithLightUvs(quad, std::span(corners).first(5)).Vertices.empty());
}
