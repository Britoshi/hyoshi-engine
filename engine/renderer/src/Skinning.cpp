#include "renderer/Skinning.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hyoshi::renderer
{
namespace
{

struct NodePose
{
    glm::vec3 Translation;
    glm::quat Rotation;
    glm::vec3 Scale;
};

// The channel's value at `time`: its first key's before it, its last's after.
glm::vec4 Sample(const AnimationChannel& channel, float time)
{
    const auto after = std::ranges::upper_bound(channel.Times, time);
    if (after == channel.Times.begin())
    {
        return channel.Values.front();
    }
    if (after == channel.Times.end())
    {
        return channel.Values.back();
    }
    const auto next = static_cast<size_t>(after - channel.Times.begin());
    const glm::vec4& from = channel.Values[next - 1];
    const glm::vec4& to = channel.Values[next];
    const float span = channel.Times[next] - channel.Times[next - 1];
    if (channel.IsStep || span <= 0.0f)
    {
        return from;
    }
    const float t = (time - channel.Times[next - 1]) / span;
    if (channel.Path != AnimationPath::Rotation)
    {
        return glm::mix(from, to, t);
    }
    // glTF's rotations are (x, y, z, w); glm's quat constructor takes w first.
    const glm::quat a(from.w, from.x, from.y, from.z);
    glm::quat b(to.w, to.x, to.y, to.z);
    if (glm::dot(a, b) < 0.0f)
    {
        b = -b;
    }
    const glm::quat turned = glm::normalize(glm::slerp(a, b, t));
    return {turned.x, turned.y, turned.z, turned.w};
}

glm::mat4 ToMatrix(const NodePose& pose)
{
    glm::mat4 matrix = glm::mat4_cast(pose.Rotation);
    matrix[0] *= pose.Scale.x;
    matrix[1] *= pose.Scale.y;
    matrix[2] *= pose.Scale.z;
    matrix[3] = glm::vec4(pose.Translation, 1.0f);
    return matrix;
}

// A vertex's joints' matrices blended by weight; none if no joint moves it.
bool BlendJoints(const VertexJoints& skin, std::span<const glm::mat4> joints, glm::mat4& blended)
{
    if (skin.Weights.x + skin.Weights.y + skin.Weights.z + skin.Weights.w <= 0.0f)
    {
        return false;
    }
    blended = glm::mat4(0.0f);
    for (int k = 0; k < 4; ++k)
    {
        const float weight = skin.Weights[k];
        const size_t joint = skin.Joints[static_cast<size_t>(k)];
        if (weight > 0.0f && joint < joints.size())
        {
            blended += joints[joint] * weight;
        }
    }
    return true;
}

} // namespace

const ModelAnimation* FindAnimation(const ModelData& model, std::string_view name)
{
    const auto found = std::ranges::find(model.Animations, name, &ModelAnimation::Name);
    return found != model.Animations.end() ? &*found : nullptr;
}

void PoseNodes(const ModelData& model, const ModelAnimation* animation, float time, std::vector<glm::mat4>& nodes)
{
    std::vector<NodePose> poses(model.Nodes.size());
    for (size_t i = 0; i < model.Nodes.size(); ++i)
    {
        const ModelNode& node = model.Nodes[i];
        poses[i] = {node.Translation, node.Rotation, node.Scale};
    }
    if (animation != nullptr)
    {
        const float at = animation->Duration > 0.0f ? std::fmod(std::max(time, 0.0f), animation->Duration) : 0.0f;
        for (const AnimationChannel& channel : animation->Channels)
        {
            if (channel.Node >= poses.size() || channel.Times.empty())
            {
                continue;
            }
            const glm::vec4 value = Sample(channel, at);
            NodePose& pose = poses[channel.Node];
            switch (channel.Path)
            {
            case AnimationPath::Translation:
                pose.Translation = glm::vec3(value);
                break;
            case AnimationPath::Rotation:
                pose.Rotation = glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
                break;
            case AnimationPath::Scale:
                pose.Scale = glm::vec3(value);
                break;
            }
        }
    }
    // Parents come before their children, so each parent's is ready.
    nodes.resize(model.Nodes.size());
    for (size_t i = 0; i < model.Nodes.size(); ++i)
    {
        const int32_t parent = model.Nodes[i].Parent;
        const glm::mat4 own = ToMatrix(poses[i]);
        nodes[i] = parent >= 0 ? nodes[static_cast<size_t>(parent)] * own : own;
    }
}

void PoseJoints(const ModelData& model, std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints)
{
    joints.resize(model.Joints.size());
    for (size_t i = 0; i < model.Joints.size(); ++i)
    {
        const ModelJoint& joint = model.Joints[i];
        joints[i] = (joint.Node < nodes.size() ? nodes[joint.Node] : glm::mat4(1.0f)) * joint.InverseBind;
    }
}

void SkinVertices(std::span<const MeshVertex> vertices, std::span<const VertexJoints> skin,
                  std::span<const glm::mat4> joints, std::span<MeshVertex> out)
{
    const size_t count = std::min({vertices.size(), skin.size(), out.size()});
    glm::mat4 blended(1.0f);
    for (size_t i = 0; i < count; ++i)
    {
        const MeshVertex& vertex = vertices[i];
        MeshVertex& moved = out[i];
        moved = vertex;
        if (!BlendJoints(skin[i], joints, blended))
        {
            continue;
        }
        moved.Position = glm::vec3(blended * glm::vec4(vertex.Position, 1.0f));
        const glm::vec3 normal = glm::mat3(blended) * vertex.Normal;
        const float length = glm::length(normal);
        moved.Normal = length > 0.0f ? normal / length : vertex.Normal;
    }
}

void ApplyPose(ModelData& model, const ModelAnimation* animation, float time)
{
    std::vector<glm::mat4> nodes;
    std::vector<glm::mat4> joints;
    PoseNodes(model, animation, time, nodes);
    PoseJoints(model, nodes, joints);
    for (ModelPart& part : model.Parts)
    {
        if (part.Skin.empty())
        {
            continue;
        }
        std::vector<MeshVertex>& vertices = part.Mesh.Vertices;
        // Blend shapes move a vertex where it was bound: turned with it.
        for (MorphTarget& morph : part.Morphs)
        {
            for (auto& [index, move] : morph.Moves)
            {
                glm::mat4 blended(1.0f);
                if (index < part.Skin.size() && BlendJoints(part.Skin[index], joints, blended))
                {
                    move = glm::mat3(blended) * move;
                }
            }
        }
        std::vector<MeshVertex> posed(vertices.size());
        SkinVertices(vertices, part.Skin, joints, posed);
        vertices = std::move(posed);
        part.Skin.clear();
    }
}

} // namespace hyoshi::renderer
