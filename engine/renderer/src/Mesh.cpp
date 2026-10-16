#include "renderer/Mesh.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace hyoshi::renderer
{
namespace
{

// How far apart, in u and v, the points are that find a surface's normal.
constexpr float NORMAL_STEP = 1.0e-3f;

// A grid of (columns + 1) x (rows + 1) points, two triangles to each cell, the front seen with u to
// the right and v down. `normalAt` gives the normal at (u, v), or, empty, it's found from the
// neighboring points.
void AddGrid(MeshData& mesh, const glm::mat4& transform, uint32_t columns, uint32_t rows,
             const std::function<glm::vec3(glm::vec2)>& surface, const std::function<glm::vec3(glm::vec2)>& normalAt,
             Color color)
{
    columns = std::max(columns, 1U);
    rows = std::max(rows, 1U);
    const glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(transform)));
    const uint32_t packed = PackMeshColor(color);
    const auto first = static_cast<uint32_t>(mesh.Vertices.size());
    for (uint32_t row = 0; row <= rows; ++row)
    {
        for (uint32_t column = 0; column <= columns; ++column)
        {
            const glm::vec2 uv{static_cast<float>(column) / static_cast<float>(columns),
                               static_cast<float>(row) / static_cast<float>(rows)};
            glm::vec3 normal{0.0f};
            if (normalAt)
            {
                normal = normalAt(uv);
            }
            else
            {
                // The front is where the texture reads the right way round: v down, u to the right.
                const glm::vec3 alongU =
                    surface(uv + glm::vec2(NORMAL_STEP, 0.0f)) - surface(uv - glm::vec2(NORMAL_STEP, 0.0f));
                const glm::vec3 alongV =
                    surface(uv + glm::vec2(0.0f, NORMAL_STEP)) - surface(uv - glm::vec2(0.0f, NORMAL_STEP));
                normal = glm::cross(alongV, alongU);
            }
            const glm::vec3 turned = normalTransform * normal;
            const float length = glm::length(turned);
            MeshVertex vertex;
            vertex.Position = glm::vec3(transform * glm::vec4(surface(uv), 1.0f));
            vertex.Normal = length > 0.0f ? turned / length : glm::vec3(0.0f, 1.0f, 0.0f);
            vertex.Uv = uv;
            vertex.Color = packed;
            mesh.Vertices.push_back(vertex);
        }
    }
    for (uint32_t row = 0; row < rows; ++row)
    {
        for (uint32_t column = 0; column < columns; ++column)
        {
            const uint32_t topLeft = first + (row * (columns + 1)) + column;
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + columns + 1;
            const uint32_t bottomRight = bottomLeft + 1;
            mesh.Indices.insert(mesh.Indices.end(), {topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight});
        }
    }
}

} // namespace

uint32_t PackMeshColor(Color color)
{
    return uint32_t{color.R} | (uint32_t{color.G} << 8) | (uint32_t{color.B} << 16) | (uint32_t{color.A} << 24);
}

void AddSurface(MeshData& mesh, const glm::mat4& transform, uint32_t columns, uint32_t rows,
                const std::function<glm::vec3(glm::vec2)>& surface, Color color)
{
    AddGrid(mesh, transform, columns, rows, surface, {}, color);
}

void AddPlane(MeshData& mesh, const glm::mat4& transform, uint32_t columns, uint32_t rows, Color color)
{
    AddSurface(
        mesh, transform, columns, rows, [](glm::vec2 uv) { return glm::vec3(uv.x - 0.5f, 0.0f, uv.y - 0.5f); }, color);
}

void AddBox(MeshData& mesh, const glm::mat4& transform, Color color)
{
    // Each face seen from outside, its texture upright (the top and bottom's tops toward -z and +z).
    const std::array<std::function<glm::vec3(glm::vec2)>, 6> faces{
        [](glm::vec2 uv) { return glm::vec3(uv.x - 0.5f, 0.5f, uv.y - 0.5f); },  // +y
        [](glm::vec2 uv) { return glm::vec3(uv.x - 0.5f, -0.5f, 0.5f - uv.y); }, // -y
        [](glm::vec2 uv) { return glm::vec3(uv.x - 0.5f, 0.5f - uv.y, 0.5f); },  // +z
        [](glm::vec2 uv) { return glm::vec3(0.5f - uv.x, 0.5f - uv.y, -0.5f); }, // -z
        [](glm::vec2 uv) { return glm::vec3(0.5f, 0.5f - uv.y, 0.5f - uv.x); },  // +x
        [](glm::vec2 uv) { return glm::vec3(-0.5f, 0.5f - uv.y, uv.x - 0.5f); }, // -x
    };
    for (const std::function<glm::vec3(glm::vec2)>& face : faces)
    {
        AddSurface(mesh, transform, 1, 1, face, color);
    }
}

void AddCylinder(MeshData& mesh, const glm::mat4& transform, uint32_t sides, Color color, bool isCapped)
{
    sides = std::max(sides, 3U);
    const auto around = [](float u)
    {
        const float angle = u * glm::two_pi<float>();
        return glm::vec2(0.5f * std::sin(angle), 0.5f * std::cos(angle));
    };
    AddGrid(
        mesh, transform, sides, 1,
        [&around](glm::vec2 uv)
        {
            const glm::vec2 point = around(uv.x);
            return glm::vec3(point.x, 0.5f - uv.y, point.y);
        },
        [&around](glm::vec2 uv)
        {
            const glm::vec2 point = around(uv.x);
            return glm::vec3(point.x, 0.0f, point.y);
        },
        color);
    if (!isCapped)
    {
        return;
    }
    const glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(transform)));
    const uint32_t packed = PackMeshColor(color);
    for (const float side : {0.5f, -0.5f})
    {
        const glm::vec3 normal = glm::normalize(normalTransform * glm::vec3(0.0f, side * 2.0f, 0.0f));
        const auto center = static_cast<uint32_t>(mesh.Vertices.size());
        mesh.Vertices.push_back(
            {glm::vec3(transform * glm::vec4(0.0f, side, 0.0f, 1.0f)), normal, {0.5f, 0.5f}, packed});
        for (uint32_t i = 0; i <= sides; ++i)
        {
            const glm::vec2 point = around(static_cast<float>(i) / static_cast<float>(sides));
            mesh.Vertices.push_back({glm::vec3(transform * glm::vec4(point.x, side, point.y, 1.0f)), normal,
                                     glm::vec2(0.5f) + point, packed});
        }
        for (uint32_t i = 0; i < sides; ++i)
        {
            const uint32_t here = center + 1 + i;
            // Counter-clockwise seen from above for the top, from below for the bottom.
            if (side > 0.0f)
            {
                mesh.Indices.insert(mesh.Indices.end(), {center, here, here + 1});
            }
            else
            {
                mesh.Indices.insert(mesh.Indices.end(), {center, here + 1, here});
            }
        }
    }
}

void AddSphere(MeshData& mesh, const glm::mat4& transform, uint32_t segments, uint32_t rings, Color color)
{
    const auto unit = [](glm::vec2 uv)
    {
        const float around = uv.x * glm::two_pi<float>();
        const float down = uv.y * glm::pi<float>();
        return glm::vec3(std::sin(down) * std::sin(around), std::cos(down), std::sin(down) * std::cos(around));
    };
    AddGrid(
        mesh, transform, std::max(segments, 3U), std::max(rings, 2U), [&unit](glm::vec2 uv) { return 0.5f * unit(uv); },
        unit, color);
}

} // namespace hyoshi::renderer
