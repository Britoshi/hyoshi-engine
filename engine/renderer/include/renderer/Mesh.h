#pragma once

#include "renderer/SpriteBatch.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace hyoshi::renderer
{

// A 3D mesh's vertex (decision 0003), 36 bytes: where it is, which way its surface faces, where it
// samples the material's texture, and a color that multiplies the material's.
struct MeshVertex
{
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec2 Uv{0.0f};
    // RGBA8, red first in memory, as SpriteBatch packs colors.
    uint32_t Color = 0xFFFFFFFF;
};

// Triangles, three indices each, wound counter-clockwise seen from their front.
struct MeshData
{
    std::vector<MeshVertex> Vertices;
    std::vector<uint32_t> Indices;
};

uint32_t PackMeshColor(Color color);

// Shapes for meshes made in code. Each adds to `mesh`, moved, turned, and sized by `transform`
// (normals follow), wound counter-clockwise seen from outside.

// A cube from -0.5 to 0.5 on each axis, each face its own texture square (0 to 1).
void AddBox(MeshData& mesh, const glm::mat4& transform, Color color = WHITE);

// A cylinder along y, of radius 0.5 from y -0.5 to 0.5, with `sides` sides and, if `isCapped`, its
// ends closed. The texture wraps around it once (u) from bottom to top (v).
void AddCylinder(MeshData& mesh, const glm::mat4& transform, uint32_t sides, Color color = WHITE, bool isCapped = true);

// A sphere of radius 0.5, in `segments` around and `rings` from pole to pole.
void AddSphere(MeshData& mesh, const glm::mat4& transform, uint32_t segments, uint32_t rings, Color color = WHITE);

// A surface over the texture's square: `surface` gives the point at each (u, v) from 0 to 1, in
// `columns` by `rows` quads. Its front is the side the texture reads the right way round from (u to
// the right, v down). Normals are found from the neighboring points, so bends are lit smoothly.
void AddSurface(MeshData& mesh, const glm::mat4& transform, uint32_t columns, uint32_t rows,
                const std::function<glm::vec3(glm::vec2)>& surface, Color color = WHITE);

// A flat square from -0.5 to 0.5 in x and z, facing up (+y), its texture's top toward -z.
void AddPlane(MeshData& mesh, const glm::mat4& transform, uint32_t columns = 1, uint32_t rows = 1, Color color = WHITE);

// `mesh` cut into `count` slabs across `axis` (a unit vector), evenly from `from` to `to` along it:
// slab i holds what lies between the i-th cut and the next, the first slab everything before `from`
// too and the last everything past `to`. Triangles that cross a cut are split there, the new
// points' normals, texture coordinates, and colors in between. For effects that pull a shape apart,
// such as a glitch's slices.
std::vector<MeshData> SliceMesh(const MeshData& mesh, glm::vec3 axis, float from, float to, uint32_t count);

} // namespace hyoshi::renderer
