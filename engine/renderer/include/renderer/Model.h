#pragma once

#include "core/Result.h"
#include "renderer/Image.h"
#include "renderer/Mesh.h"
#include "renderer/SpriteBatch.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace hyoshi::renderer
{

// A part of a model: its triangles with one material.
struct ModelPart
{
    // The material's name in the file.
    std::string Name;
    MeshData Mesh;
    // The material's base color (sRGB, as Material::BaseColor), and its base color texture, if any
    // (RGBA8; empty when none).
    Color BaseColor = WHITE;
    Image Texture;
    // Both faces drawn, and seen through by its alpha (glTF's BLEND and MASK), as the file says.
    bool IsTwoSided = false;
    bool IsSeeThrough = false;
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
// sidedness, and blending. glTF's axes are the engine's (+y up, right-handed, a model's front toward
// +z), in meters. A skinned mesh comes in its rest pose, logged: pose it in Blender and apply the
// armature before exporting.
Result<ModelData> ReadModel(std::span<const std::byte> data, const std::string& path = {});

// Reads a model from a file (desktop builds; elsewhere read the file's bytes and use ReadModel).
Result<ModelData> LoadModel(const std::string& path);

} // namespace hyoshi::renderer
