#pragma once

#include "renderer/Mesh.h"
#include "renderer/Model.h"

#include <glm/mat4x4.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace hyoshi::renderer
{

// Posing a skinned model (renderer/Model.h's skins and animations) on the CPU: its nodes posed, then
// its skinned parts' vertices moved by their joints, to draw as a dynamic mesh
// (Renderer3D::CreateDynamicMesh, UpdateMesh) or to set into the model for good (ApplyPose).

// The model's animation by name; none if it hasn't one.
const ModelAnimation* FindAnimation(const ModelData& model, std::string_view name);

// Each of the model's nodes' transforms in the model's space, at `time` (seconds) into `animation`
// (looped: past its duration it starts again), or at rest with none. `nodes` is resized to fit.
void PoseNodes(const ModelData& model, const ModelAnimation* animation, float time, std::vector<glm::mat4>& nodes);

// Each of the model's joints' matrices for skinning, from its posed nodes (PoseNodes): its node's
// transform times its inverse bind. `joints` is resized to fit.
void PoseJoints(const ModelData& model, std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints);

// Vertices moved by their joints (`skin`, one for each vertex; `joints` from PoseJoints) into `out`
// (as long as `vertices`): positions by their joints' matrices blended by weight, normals turned as
// they are; the rest copied. A vertex no joint moves stays where it is.
void SkinVertices(std::span<const MeshVertex> vertices, std::span<const VertexJoints> skin,
                  std::span<const glm::mat4> joints, std::span<MeshVertex> out);

// The model's skinned parts set in a pose for good (their vertices and blend shapes moved, their
// joints dropped), at `time` into `animation` or at rest with none: for a model that's drawn still.
void ApplyPose(ModelData& model, const ModelAnimation* animation = nullptr, float time = 0.0f);

} // namespace hyoshi::renderer
