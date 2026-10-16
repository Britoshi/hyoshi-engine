#pragma once

#include "core/Result.h"
#include "renderer/Image.h"
#include "renderer/Mesh.h"
#include "renderer/SpriteBatch.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
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

// The joints a skinned vertex moves with (up to four: indices into ModelData::Joints) and how much
// each moves it (summing to 1; all 0 for a vertex no joint moves, which stays where it is).
struct VertexJoints
{
    std::array<uint16_t, 4> Joints{};
    glm::vec4 Weights{0.0f};
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
    // Each vertex's joints, when the part is skinned (empty when it isn't): its vertices and blend
    // shapes are then where the mesh was bound to its joints (glTF's bind pose), for PoseModel and
    // SkinVertices (renderer/Skinning.h) to move.
    std::vector<VertexJoints> Skin;
};

// A node of the model's file (an armature's bones are nodes, in glTF): its parent (an index into
// ModelData::Nodes, before it; -1 for none) and its own transform at rest.
struct ModelNode
{
    std::string Name;
    int32_t Parent = -1;
    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
};

// A joint a skinned vertex moves with: its node, and the inverse of the node's transform when the
// mesh was bound to it.
struct ModelJoint
{
    uint32_t Node = 0;
    glm::mat4 InverseBind{1.0f};
};

// What an animation sets of a node's transform, and how it goes between its keys.
enum class AnimationPath : uint8_t
{
    Translation,
    Rotation,
    Scale
};

// One node's translation, rotation (x, y, z, w), or scale over time: its keys' times (seconds,
// rising) and values; stepping from key to key, or straight between them (rotations the short way
// round). glTF's cubic splines are read as straight lines through their keys.
struct AnimationChannel
{
    uint32_t Node = 0;
    AnimationPath Path = AnimationPath::Translation;
    bool IsStep = false;
    std::vector<float> Times;
    std::vector<glm::vec4> Values;
};

// A named animation (Blender's actions are, one each): its channels, and how long it is (its last
// key's time).
struct ModelAnimation
{
    std::string Name;
    float Duration = 0.0f;
    std::vector<AnimationChannel> Channels;
};

// A model as its file's scene places it: every mesh, each node's transform applied (but a skinned
// one's: its joints place it), gathered into one part per material; its nodes, parents first, its
// skins' joints, and its animations.
struct ModelData
{
    std::vector<ModelPart> Parts;
    std::vector<ModelNode> Nodes;
    std::vector<ModelJoint> Joints;
    std::vector<ModelAnimation> Animations;
};

// Reads a glTF 2.0 model (decision 0003): a .glb, or a .gltf whose buffers and images are inside it
// (data: URIs) or, with `path`, beside it. Triangles only; positions, normals (made when missing),
// texture coordinates, and vertex colors; base colors and their textures (PNG or JPEG), double
// sidedness, and blending; blend shapes (morph targets' positions); skins (four joints a vertex) and
// animations of nodes' transforms (which move skinned meshes only: a mesh that isn't skinned stays
// where the scene places it). glTF's axes are the engine's (+y up, right-handed, a model's front
// toward +z), in meters.
Result<ModelData> ReadModel(std::span<const std::byte> data, const std::string& path = {});

// Reads a model from a file (desktop builds; elsewhere read the file's bytes and use ReadModel).
Result<ModelData> LoadModel(const std::string& path);

} // namespace hyoshi::renderer
