#include "renderer/Mesh.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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

// A vertex between `a` and `b`, `t` of the way.
MeshVertex Interpolate(const MeshVertex& a, const MeshVertex& b, float t)
{
    MeshVertex between;
    between.Position = glm::mix(a.Position, b.Position, t);
    const glm::vec3 normal = glm::mix(a.Normal, b.Normal, t);
    between.Normal = glm::length(normal) > 0.0f ? glm::normalize(normal) : a.Normal;
    between.Uv = glm::mix(a.Uv, b.Uv, t);
    between.Color = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8)
    {
        const auto channelA = static_cast<float>((a.Color >> shift) & 0xFFU);
        const auto channelB = static_cast<float>((b.Color >> shift) & 0xFFU);
        const auto channel = static_cast<uint32_t>(std::lround(channelA + ((channelB - channelA) * t)));
        between.Color |= std::min(channel, 0xFFU) << shift;
    }
    return between;
}

// What of `polygon` lies where `distance` (along the slicing axis) is at least `limit` times
// `side` (1: keep from the limit on; -1: keep up to it), in order, cut where it crosses.
std::vector<MeshVertex> ClipPolygon(const std::vector<MeshVertex>& polygon, glm::vec3 axis, float limit, float side)
{
    std::vector<MeshVertex> kept;
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const MeshVertex& current = polygon[i];
        const MeshVertex& next = polygon[(i + 1) % polygon.size()];
        const float currentInside = (glm::dot(current.Position, axis) - limit) * side;
        const float nextInside = (glm::dot(next.Position, axis) - limit) * side;
        if (currentInside >= 0.0f)
        {
            kept.push_back(current);
        }
        if ((currentInside >= 0.0f) != (nextInside >= 0.0f))
        {
            kept.push_back(Interpolate(current, next, currentInside / (currentInside - nextInside)));
        }
    }
    return kept;
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

std::vector<MeshData> SliceMesh(const MeshData& mesh, glm::vec3 axis, float from, float to, uint32_t count)
{
    count = std::max(count, 1U);
    std::vector<MeshData> slabs(count);
    const float step = (to - from) / static_cast<float>(count);
    for (size_t index = 0; index + 2 < mesh.Indices.size(); index += 3)
    {
        const std::vector<MeshVertex> triangle{mesh.Vertices[mesh.Indices[index]],
                                               mesh.Vertices[mesh.Indices[index + 1]],
                                               mesh.Vertices[mesh.Indices[index + 2]]};
        for (uint32_t slab = 0; slab < count; ++slab)
        {
            std::vector<MeshVertex> piece = triangle;
            if (slab > 0)
            {
                piece = ClipPolygon(piece, axis, from + (step * static_cast<float>(slab)), 1.0f);
            }
            if (slab + 1 < count && piece.size() >= 3)
            {
                piece = ClipPolygon(piece, axis, from + (step * static_cast<float>(slab + 1)), -1.0f);
            }
            if (piece.size() < 3)
            {
                continue;
            }
            // A fan keeps the triangle's winding.
            MeshData& out = slabs[slab];
            const auto first = static_cast<uint32_t>(out.Vertices.size());
            out.Vertices.insert(out.Vertices.end(), piece.begin(), piece.end());
            for (uint32_t corner = 1; corner + 1 < piece.size(); ++corner)
            {
                out.Indices.insert(out.Indices.end(), {first, first + corner, first + corner + 1});
            }
        }
    }
    return slabs;
}

} // namespace hyoshi::renderer
