#pragma once

#include "renderer/Model.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hyoshi::renderer
{

// How a chain of bones swings (as VRM's spring bones have it): how hard it's pulled back toward where
// the animation points it, how much of its speed it loses each step (0 to 1), and how hard it's
// pulled down. The pulls are meters a second at the bone's tip: 1 moves it about 1.7 cm a step. How
// thick its tips are (meters): kept that far out of the colliders, and above the floor.
struct SpringSettings
{
    float Stiffness = 1.0f;
    float Drag = 0.4f;
    float Gravity = 0.0f;
    float Radius = 0.0f;
};

// A seat's top, level, in the model's space (a chair's, a bed's): across and along (x and z) from
// `From` to `To`, `Height` up, a block down to the floor under it.
struct SpringSeat
{
    glm::vec2 From{0.0f};
    glm::vec2 To{0.0f};
    float Height = 0.0f;
};

// Bones that swing as a model moves (hair, a coat's tails, a skirt: Unity's dynamic bones), simple
// and cheap, on the CPU: each bone's tip (its child's place) carried on by its own speed, pulled
// back toward where the animation points it and down, kept out of the colliders (capsules around the
// body's bones), on a seat, and above the floor, if it has them, and at the bone's length; the bone
// turned to point at it, and the bones below it with it. In steps of a fixed length (60 a second), so
// the same motion swings the same way every time, as a clip baked from it would.
class SpringBones
{
public:
    // Swings the named node and every node below it that has one child (a chain's last bone, with
    // none, turns with its parent). Returns how many it swings: none if the model hasn't the node.
    size_t Add(const ModelData& model, std::string_view node, SpringSettings settings);

    // A capsule the swinging bones' tips keep out of: around the bone from the node `from` to the node
    // `to` (empty: a ball at `from`), as thick as `radius`. Returns false if the model hasn't them.
    bool AddCollider(const ModelData& model, std::string_view from, std::string_view to, float radius);

    // A floor the swinging bones' tips keep above (its height in the model's space; none: no floor).
    void SetFloor(std::optional<float> height)
    {
        floor = height;
    }

    // A seat the swinging bones lie on and hang over the edges of (none: no seat): a tip over it whose
    // bone comes down from its top or above is kept on its top; one beside it, under its top, out of
    // its sides.
    void SetSeat(std::optional<SpringSeat> top)
    {
        seat = top;
    }

    // The next Step starts again from the animation as it stands (after a jump, such as a new clip).
    void Reset();

    // Swings the model's posed nodes (PoseNodes's: in the model's space, +y up) on by `seconds`
    // since the last step (at most a few steps' worth).
    void Step(const ModelData& model, std::vector<glm::mat4>& nodes, float seconds);

    size_t GetCount() const
    {
        return joints.size();
    }

private:
    struct Joint
    {
        uint32_t Node = 0;
        uint32_t Child = 0;
        // The nodes below it, turned with it.
        std::vector<uint32_t> Below;
        SpringSettings Settings;
        // Where its tip is, and was a step before.
        glm::vec3 Tip{0.0f};
        glm::vec3 Before{0.0f};
    };

    // A capsule around a bone (from a node to another; to itself, a ball).
    struct Collider
    {
        uint32_t From = 0;
        uint32_t To = 0;
        float Radius = 0.0f;
    };

    // The joints, parents first, swung in `nodes`: their tips moved a step on if `isStepping`, else
    // where they are.
    void Swing(std::vector<glm::mat4>& nodes, bool isStepping);
    // `tip` (as thick as `radius`) moved out of the colliders as `nodes` have them, and above the floor.
    glm::vec3 KeepOut(const std::vector<glm::mat4>& nodes, glm::vec3 tip, float radius) const;

    std::vector<Joint> joints;
    std::vector<Collider> colliders;
    std::optional<float> floor;
    std::optional<SpringSeat> seat;
    std::vector<glm::mat4> animated;
    float pending = 0.0f;
    bool isStarted = false;
};

} // namespace hyoshi::renderer
