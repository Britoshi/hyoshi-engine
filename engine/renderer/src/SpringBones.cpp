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
constexpr glm::vec3 DOWN{0.0f, -1.0f, 0.0f};

// Where `tip` (as thick as `radius`, its bone from `head`, `length` long) goes out of the seat, if
// it's in it (over it, as far as it's thick, and under its top), by the way its bone came in: through
// the top (from over the seat, at or above its top, or crossing its edge there), onto the top at its
// length, the same way round (straight down: toward the nearest side), so the next bone's head is no
// lower than the top; through a side (crossing its edge under the top), out of that side.
std::optional<glm::vec3> GetOutOfSeat(const SpringSeat& seat, glm::vec3 head, glm::vec3 tip, float length, float radius)
{
    const glm::vec2 from = seat.From - glm::vec2(radius);
    const glm::vec2 to = seat.To + glm::vec2(radius);
    const float top = seat.Height + radius;
    if (tip.x <= from.x || tip.x >= to.x || tip.z <= from.y || tip.z >= to.y || tip.y >= top)
    {
        return std::nullopt;
    }
    // Where along the bone it crosses the seat's edge (from beside it: x's low and high sides, then
    // z's), and which side.
    float crossing = 0.0f;
    std::optional<size_t> crossed;
    const auto cross = [&crossing, &crossed](float bone, float end, float low, float high, size_t lowSide)
    {
        const bool isLow = bone < low;
        if (isLow || bone > high)
        {
            const float at = ((isLow ? low : high) - bone) / (end - bone);
            if (at > crossing)
            {
                crossing = at;
                crossed = isLow ? lowSide : lowSide + 1;
            }
        }
    };
    cross(head.x, tip.x, from.x, to.x, 0);
    cross(head.z, tip.z, from.y, to.y, 2);
    const std::array<float, 4> out{tip.x - from.x, to.x - tip.x, tip.z - from.y, to.y - tip.z};
    const auto nearest = static_cast<size_t>(std::ranges::min_element(out) - out.begin());
    const bool isFromTop = crossed ? head.y + (crossing * (tip.y - head.y)) >= top : head.y >= seat.Height;
    if (isFromTop)
    {
        constexpr std::array<glm::vec2, 4> TO_SIDE{{{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}}};
        const float drop = std::clamp(head.y - top, 0.0f, length);
        const glm::vec2 flat(tip.x - head.x, tip.z - head.z);
        const float across = glm::length(flat);
        const glm::vec2 way = across > 1.0e-6f ? flat / across : TO_SIDE[nearest];
        const glm::vec2 lying = way * std::sqrt((length * length) - (drop * drop));
        return glm::vec3(head.x + lying.x, head.y - drop, head.z + lying.y);
    }
    switch (crossed.value_or(nearest))
    {
    case 0:
        return glm::vec3(from.x, tip.y, tip.z);
    case 1:
        return glm::vec3(to.x, tip.y, tip.z);
    case 2:
        return glm::vec3(tip.x, tip.y, from.y);
    default:
        return glm::vec3(tip.x, tip.y, to.y);
    }
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
    if (!isStarted)
    {
        for (Joint& joint : joints)
        {
            joint.Tip = glm::vec3(nodes[joint.Child][3]);
            joint.Before = joint.Tip;
        }
        isStarted = true;
    }
    pending = std::min(pending + std::max(seconds, 0.0f), STEP_SECONDS * static_cast<float>(MAX_STEPS));
    // Each step from the animation as it stands; between steps (a frame shorter than one), the tips
    // stay where they are.
    animated = nodes;
    bool hasStepped = false;
    while (pending >= STEP_SECONDS)
    {
        pending -= STEP_SECONDS;
        nodes = animated;
        Swing(nodes, true);
        hasStepped = true;
    }
    if (!hasStepped)
    {
        Swing(nodes, false);
    }
}

void SpringBones::Swing(std::vector<glm::mat4>& nodes, bool isStepping)
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
        if (isStepping)
        {
            const glm::vec3 next = joint.Tip + ((joint.Tip - joint.Before) * (1.0f - settings.Drag)) +
                                   (aim * (settings.Stiffness * STEP_SECONDS)) +
                                   (DOWN * (settings.Gravity * STEP_SECONDS));
            joint.Before = joint.Tip;
            joint.Tip = KeepOut(nodes, next, settings.Radius);
        }
        const glm::vec3 toTip = joint.Tip - head;
        const float reach = glm::length(toTip);
        glm::vec3 pointing = reach > 1.0e-6f ? toTip / reach : aim;
        joint.Tip = head + (pointing * length);
        // On the seat, or out of its side.
        if (const std::optional<glm::vec3> out =
                seat ? GetOutOfSeat(*seat, head, joint.Tip, length, settings.Radius) : std::nullopt)
        {
            joint.Tip = *out;
            const glm::vec3 toOut = joint.Tip - head;
            const float outReach = glm::length(toOut);
            pointing = outReach > 1.0e-6f ? toOut / outReach : aim;
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

glm::vec3 SpringBones::KeepOut(const std::vector<glm::mat4>& nodes, glm::vec3 tip, float radius) const
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
    if (floor)
    {
        tip.y = std::max(tip.y, *floor + radius);
    }
    return tip;
}

} // namespace hyoshi::renderer
