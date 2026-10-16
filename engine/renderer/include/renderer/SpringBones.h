#pragma once

#include "renderer/Model.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hyoshi::renderer
{

// How a chain of bones swings (as VRM's spring bones have it): how hard it's pulled back toward where
// the animation points it, how much of its speed it loses each step (0 to 1), and how hard it's
// pulled down. The pulls are meters a second at the bone's tip: 1 moves it about 1.7 cm a step.
struct SpringSettings
{
    float Stiffness = 1.0f;
    float Drag = 0.4f;
    float Gravity = 0.0f;
};

// Bones that swing as a model moves (hair, a coat's tails, a skirt: Unity's dynamic bones), simple
// and cheap, on the CPU: each bone's tip (its child's place) carried on by its own speed, pulled
// back toward where the animation points it and down, kept at the bone's length; the bone turned to
// point at it, and the bones below it with it. No collisions. In steps of a fixed length (60 a
// second), so the same motion swings the same way every time, as a clip baked from it would.
class SpringBones
{
public:
    // Swings the named node and every node below it that has one child (a chain's last bone, with
    // none, turns with its parent). Returns how many it swings: none if the model hasn't the node.
    size_t Add(const ModelData& model, std::string_view node, SpringSettings settings);

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

    // The joints, parents first, swung in `nodes`: their tips moved a step on if `isStepping`, else
    // where they are.
    void Swing(std::vector<glm::mat4>& nodes, bool isStepping);

    std::vector<Joint> joints;
    std::vector<glm::mat4> animated;
    float pending = 0.0f;
    bool isStarted = false;
};

} // namespace hyoshi::renderer
