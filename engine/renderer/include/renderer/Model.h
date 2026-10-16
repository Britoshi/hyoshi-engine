#pragma once

#include "core/Result.h"
#include "renderer/Image.h"
#include "renderer/Mesh.h"
#include "renderer/SpriteBatch.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hyoshi::renderer
{

// A blend shape (glTF's morph target): its name, and where it moves the vertices it moves, at its
// fullest (each vertex's index in its part, and how far; a vertex it doesn't move isn't listed).
struct MorphTarget
{
    std::string Name;
    std::vector<std::pair<uint32_t, glm::vec3>> Moves;
};

// A part of a model: its triangles with one material.
struct ModelPart
{
    // The material's name in the file, and its extras (JSON, as the file has them: Blender's custom
    // properties; empty when none).
    std::string Name;
    std::string Extras;
    MeshData Mesh;
    // The material's base color (sRGB, as Material::BaseColor), and its base color texture, if any
    // (RGBA8; empty when none).
    Color BaseColor = WHITE;
    Image Texture;
    // Both faces drawn, and seen through by its alpha (glTF's BLEND and MASK), as the file says.
    bool IsTwoSided = false;
    bool IsSeeThrough = false;
    // Its blend shapes, by name (the meshes' target names, as Blender exports its shape keys), their
    // positions only.
    std::vector<MorphTarget> Morphs;
};

// A model as its file's scene places it: every mesh, each node's transform applied, gathered into
// one part per material.
struct ModelData
{
    std::vector<ModelPart> Parts;
};

// Reads a glTF 2.0 model (decision 0003): a .glb, or a .gltf whose buffers and images are inside it
// (data: URIs) or, with `path`, beside it. Triangles only; positions, normals (made when missing),
// texture coordinates, and vertex colors; base colors and their textures (PNG or JPEG), double
// sidedness, and blending; blend shapes (morph targets' positions). glTF's axes are the engine's (+y up, right-handed,
// a model's front toward +z), in meters. A skinned mesh comes in its rest pose, logged: pose it in Blender and apply
// the armature before exporting.
Result<ModelData> ReadModel(std::span<const std::byte> data, const std::string& path = {});

// Reads a model from a file (desktop builds; elsewhere read the file's bytes and use ReadModel).
Result<ModelData> LoadModel(const std::string& path);

} // namespace hyoshi::renderer
