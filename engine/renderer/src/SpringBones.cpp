#include "renderer/SpringBones.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace hyoshi::renderer
{
namespace
{

constexpr float STEP_SECONDS = 1.0f / 60.0f;
// Steps a Step may take, so a long frame (or a pause) doesn't run the swinging on and on.
constexpr int32_t MAX_STEPS = 4;
// Steps swung, the model held as it stands, as a Step starts again (Reset), so the bones are already
// hanging as they come to rest, not swinging from where the animation points them.
constexpr int32_t SETTLE_STEPS = 60;
// Times a step puts a tip at its bone's length and then out of what it touches, so neither undoes the
// other (each can move it back into the other's way).
constexpr int32_t SOLVE_PASSES = 4;
constexpr glm::vec3 DOWN{0.0f, -1.0f, 0.0f};
// A bone's head this little under a seat's top counts as on it (a bone lying on the top ends a little
// under it at its length, and a body breathing moves it up and down).
constexpr float ON_TOP = 0.01f;

// `tip` (as thick as `radius`, its bone from `head`) kept out of the seat, and its bone with it, so a
// bone from over the seat lies on its top and over its edge, not through it. A tip in it (over it, as
// far as it's thick, and under its top) goes out through the face the bone came in by (the last of
// their planes it crosses from the head; with the head in the seat too, the nearest): onto the top,
// or out of a side. A tip beyond it whose bone goes through it, from its top out of a side, is turned
// about the head up onto the edge it goes over, resting on it.
glm::vec3 GetOutOfSeat(const SpringSeat& seat, glm::vec3 head, glm::vec3 tip, float radius)
{
    const glm::vec2 from = seat.From - glm::vec2(radius);
    const glm::vec2 to = seat.To + glm::vec2(radius);
    const float top = seat.Height + radius;
    const auto isOver = [&from, &to](glm::vec3 at)
    { return at.x > from.x && at.x < to.x && at.z > from.y && at.z < to.y; };
    // From over the top, or on it; and how far along the bone it comes down to the top.
    const bool isFromTop = head.y >= top - ON_TOP;
    const float down = head.y > top ? (top - head.y) / (tip.y - head.y) : 0.0f;
    if (isOver(tip) && tip.y < top)
    {
        const std::array<float, 5> depths{top - tip.y, tip.x - from.x, to.x - tip.x, tip.z - from.y, to.y - tip.z};
        float latest = -1.0f;
        auto face = static_cast<size_t>(std::ranges::min_element(depths) - depths.begin());
        const auto cross = [&latest, &face](float bone, float end, float low, float high, size_t lowFace)
        {
            if (bone < low || bone > high)
            {
                const float at = ((bone < low ? low : high) - bone) / (end - bone);
                if (at > latest)
                {
                    latest = at;
                    face = bone < low ? lowFace : lowFace + 1;
                }
            }
        };
        if (isFromTop)
        {
            latest = down;
            face = 0;
        }
        cross(head.x, tip.x, from.x, to.x, 1);
        cross(head.z, tip.z, from.y, to.y, 3);
        switch (face)
        {
        case 0:
            return {tip.x, top, tip.z};
        case 1:
            return {from.x, tip.y, tip.z};
        case 2:
            return {to.x, tip.y, tip.z};
        case 3:
            return {tip.x, tip.y, from.y};
        default:
            return {tip.x, tip.y, to.y};
        }
    }
    if (isFromTop && tip.y < top && isOver(head + ((tip - head) * down)))
    {
        // Where the bone leaves the seat's sides (under its top), and the edge over it.
        float leaves = 1.0f;
        const auto leave = [&leaves](float bone, float end, float low, float high)
        {
            if (end < low || end > high)
            {
                leaves = std::min(leaves, ((end < low ? low : high) - bone) / (end - bone));
            }
        };
        leave(head.x, tip.x, from.x, to.x);
        leave(head.z, tip.z, from.y, to.y);
        const glm::vec3 out = head + ((tip - head) * leaves);
        return head + (glm::normalize(glm::vec3(out.x, top, out.z) - head) * glm::length(tip - head));
    }
    return tip;
}

// The turn from one direction to another (both of length 1), the shortest way.
glm::quat TurnBetween(glm::vec3 from, glm::vec3 to)
{
    const float facing = glm::dot(from, to);
    if (facing < -0.9999f)
    {
        const glm::vec3 across = std::abs(from.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(glm::cross(from, across)));
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + facing, axis.x, axis.y, axis.z));
}

bool IsBelow(const ModelData& model, uint32_t node, uint32_t above)
{
    for (int32_t at = model.Nodes[node].Parent; at >= 0; at = model.Nodes[static_cast<size_t>(at)].Parent)
    {
        if (static_cast<uint32_t>(at) == above)
        {
            return true;
        }
    }
    return false;
}

} // namespace

size_t SpringBones::Add(const ModelData& model, std::string_view node, SpringSettings settings)
{
    const auto found = std::ranges::find(model.Nodes, node, &ModelNode::Name);
    if (found == model.Nodes.end())
    {
        return 0;
    }
    const auto top = static_cast<uint32_t>(found - model.Nodes.begin());
    const auto count = static_cast<uint32_t>(model.Nodes.size());
    std::vector<uint32_t> children(count, 0);
    std::vector<uint32_t> child(count, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (const int32_t parent = model.Nodes[i].Parent; parent >= 0)
        {
            ++children[static_cast<size_t>(parent)];
            child[static_cast<size_t>(parent)] = i;
        }
    }
    size_t added = 0;
    // Parents come before their children, so a node's descendants are after it.
    for (uint32_t i = top; i < count; ++i)
    {
        const bool isHere = i == top || IsBelow(model, i, top);
        const bool isKnown = std::ranges::any_of(joints, [i](const Joint& joint) { return joint.Node == i; });
        if (!isHere || isKnown || children[i] != 1)
        {
            continue;
        }
        Joint joint;
        joint.Node = i;
        joint.Child = child[i];
        joint.Settings = settings;
        for (uint32_t below = i + 1; below < count; ++below)
        {
            if (IsBelow(model, below, i))
            {
                joint.Below.push_back(below);
            }
        }
        joints.push_back(std::move(joint));
        ++added;
    }
    std::ranges::sort(joints, {}, &Joint::Node);
    Reset();
    return added;
}

bool SpringBones::AddCollider(const ModelData& model, std::string_view from, std::string_view to, float radius)
{
    const auto find = [&model](std::string_view name)
    { return std::ranges::find(model.Nodes, name, &ModelNode::Name); };
    const auto start = find(from);
    const auto end = to.empty() ? start : find(to);
    if (start == model.Nodes.end() || end == model.Nodes.end())
    {
        return false;
    }
    colliders.push_back(
        {static_cast<uint32_t>(start - model.Nodes.begin()), static_cast<uint32_t>(end - model.Nodes.begin()), radius});
    return true;
}

void SpringBones::Reset()
{
    isStarted = false;
    pending = 0.0f;
}

void SpringBones::Step(const ModelData& model, std::vector<glm::mat4>& nodes, float seconds)
{
    if (joints.empty() || nodes.size() < model.Nodes.size())
    {
        return;
    }
    animated = nodes;
    if (!isStarted)
    {
        for (Joint& joint : joints)
        {
            joint.Tip = glm::vec3(nodes[joint.Child][3]);
            joint.Before = joint.Tip;
            joint.Last = joint.Tip;
        }
        for (int32_t step = 0; step < SETTLE_STEPS; ++step)
        {
            nodes = animated;
            Swing(nodes, std::nullopt);
        }
        isStarted = true;
    }
    pending = std::min(pending + std::max(seconds, 0.0f), STEP_SECONDS * static_cast<float>(MAX_STEPS));
    // Each step from the animation as it stands.
    while (pending >= STEP_SECONDS)
    {
        pending -= STEP_SECONDS;
        nodes = animated;
        Swing(nodes, std::nullopt);
    }
    // Shown between the last two steps, as far as the time since the last is toward the next, so the
    // bones move every frame, at any frame rate, not only on frames that step (on a 120 Hz screen,
    // every other frame: the cloth stuttering against the body as it moves).
    nodes = animated;
    Swing(nodes, pending / STEP_SECONDS);
}

void SpringBones::Swing(std::vector<glm::mat4>& nodes, std::optional<float> shownAt)
{
    for (Joint& joint : joints)
    {
        const glm::vec3 head(nodes[joint.Node][3]);
        const glm::vec3 toTail = glm::vec3(nodes[joint.Child][3]) - head;
        const float length = glm::length(toTail);
        if (length < 1.0e-6f)
        {
            continue;
        }
        const glm::vec3 aim = toTail / length;
        const SpringSettings& settings = joint.Settings;
        glm::vec3 tip = shownAt ? glm::mix(joint.Last, joint.Tip, *shownAt)
                                : joint.Tip + ((joint.Tip - joint.Before) * (1.0f - settings.Drag)) +
                                      (aim * (settings.Stiffness * STEP_SECONDS)) +
                                      (DOWN * (settings.Gravity * STEP_SECONDS));
        bool isTouching = false;
        for (int32_t pass = 0; pass < SOLVE_PASSES; ++pass)
        {
            const glm::vec3 toTip = tip - head;
            const float reach = glm::length(toTip);
            tip = head + ((reach > 1.0e-6f ? toTip / reach : aim) * length);
            const glm::vec3 out = KeepOut(nodes, head, tip, settings.Radius);
            isTouching = isTouching || out != tip;
            tip = out;
        }
        const glm::vec3 toTip = tip - head;
        const float reach = glm::length(toTip);
        const glm::vec3 pointing = reach > 1.0e-6f ? toTip / reach : aim;
        // Touching something, it stops there: cloth doesn't bounce off, or slide on with its speed.
        if (!shownAt)
        {
            joint.Last = joint.Tip;
            joint.Tip = head + (pointing * length);
            joint.Before = isTouching ? joint.Tip : joint.Last;
        }
        const glm::mat4 swing = glm::translate(glm::mat4(1.0f), head) * glm::mat4_cast(TurnBetween(aim, pointing)) *
                                glm::translate(glm::mat4(1.0f), -head);
        nodes[joint.Node] = swing * nodes[joint.Node];
        for (const uint32_t below : joint.Below)
        {
            nodes[below] = swing * nodes[below];
        }
    }
}

glm::vec3 SpringBones::KeepOut(const std::vector<glm::mat4>& nodes, glm::vec3 head, glm::vec3 tip, float radius) const
{
    for (const Collider& collider : colliders)
    {
        // The nearest point on the bone, and out from it as far as the two are thick.
        const glm::vec3 from(nodes[collider.From][3]);
        const glm::vec3 along = glm::vec3(nodes[collider.To][3]) - from;
        const float lengthSquared = glm::dot(along, along);
        const float share =
            lengthSquared > 1.0e-12f ? std::clamp(glm::dot(tip - from, along) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        const glm::vec3 nearest = from + (along * share);
        const glm::vec3 away = tip - nearest;
        const float distance = glm::length(away);
        const float reach = collider.Radius + radius;
        if (distance < reach && distance > 1.0e-6f)
        {
            tip = nearest + (away * (reach / distance));
        }
    }
    if (seat)
    {
        tip = GetOutOfSeat(*seat, head, tip, radius);
    }
    if (floor)
    {
        tip.y = std::max(tip.y, *floor + radius);
    }
    return tip;
}

} // namespace hyoshi::renderer
