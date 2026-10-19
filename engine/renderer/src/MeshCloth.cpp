#include "renderer/MeshCloth.h"

#include "renderer/Skinning.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hyoshi::renderer
{
namespace
{

constexpr float STEP_SECONDS = 1.0f / 90.0f;
// Steps a Step may take, so a long frame (or a pause) doesn't run the cloth on and on.
constexpr int32_t MAX_STEPS = 6;
// Steps taken, the body held as it stands, as the cloth starts (again), so it's already hanging.
constexpr int32_t SETTLE_STEPS = 60;
// Times a step puts the cloth's points where all it keeps to wants them, in turn; the last few also
// keep its edges out of colliders (not only its points), so a thin limb can't pass between two.
constexpr int32_t SOLVE_PASSES = 6;
constexpr int32_t EDGE_PASSES = 2;
// How fast a point may go (meters a second), so nothing flings it far in a step.
constexpr float MOST_SPEED = 8.0f;
// How much farther from its fixed point than at rest a point may get, however it's pulled.
constexpr float TETHER = 1.1f;
// An edge touching a collider this near an end (a share of its length) leaves it to that end's point;
// and moves either end at most so many times as deep as it's in.
constexpr float EDGE_ENDS = 0.1f;
constexpr float MOST_EDGE_PUSH = 3.0f;
// However it's pushed, a point ends a step no farther than this (meters) past its reach (a blow-up
// caught, not a limit), and is put back where the animation has it if it isn't a number.
constexpr float PAST_REACH = 0.5f;
// The body jumping this far (meters) in a frame, or turning this far (its cosine: 60 degrees), starts
// the cloth again where the animation puts it, instead of flinging it.
constexpr float JUMP = 0.5f;
constexpr float JUMP_TURN = 0.5f;
// Vertices this close are one place (split at a seam in the texture, they're one there).
constexpr float WELD = 1.0e-4f;
// Fixed points kept: those this few edges from a moving one (the rest are drawn as they were).
constexpr int32_t FIXED_RINGS = 2;
// Passes finding a moving point's turn from the last frame's (Müller et al., "A Robust Method to
// Extract the Rotational Part of Deformations", 2016), and from the animation's, as it starts.
constexpr int32_t TURN_PASSES = 4;
constexpr int32_t START_TURN_PASSES = 24;
constexpr glm::vec3 DOWN{0.0f, -1.0f, 0.0f};
constexpr float TINY = 1.0e-7f;

// A stiffness for each pass from one for the whole step (0 to 1): as hard after all the passes.
float PerPass(float stiffness, int32_t passes)
{
    return 1.0f - std::pow(1.0f - std::clamp(stiffness, 0.0f, 1.0f), 1.0f / static_cast<float>(passes));
}

uint64_t PackCell(glm::ivec3 cell)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(cell.x) & 0x1FFFFFU) << 42U) |
           (static_cast<uint64_t>(static_cast<uint32_t>(cell.y) & 0x1FFFFFU) << 21U) |
           static_cast<uint64_t>(static_cast<uint32_t>(cell.z) & 0x1FFFFFU);
}

glm::ivec3 GetCell(glm::vec3 at, float size)
{
    return glm::ivec3(glm::floor(at / size));
}

uint64_t PackPair(uint32_t a, uint32_t b)
{
    return (static_cast<uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
}

// Fixed takes a place over moving, and moving over ignored.
int32_t GetHold(ClothPaint paint)
{
    switch (paint)
    {
    case ClothPaint::Fixed:
        return 2;
    case ClothPaint::Move:
        return 1;
    case ClothPaint::Ignore:
        break;
    }
    return 0;
}

// Joints gathered from several vertices, each moving them all so much.
class JointSum
{
public:
    void Add(const VertexJoints& joints)
    {
        for (size_t k = 0; k < joints.Joints.size(); ++k)
        {
            const float weight = joints.Weights[static_cast<glm::length_t>(k)];
            if (weight <= 0.0f)
            {
                continue;
            }
            const auto found = std::ranges::find(sums, joints.Joints[k], &std::pair<uint16_t, float>::first);
            if (found != sums.end())
            {
                found->second += weight;
            }
            else
            {
                sums.emplace_back(joints.Joints[k], weight);
            }
        }
    }

    // The four that move them most, their weights summing to 1.
    VertexJoints GetMost() const
    {
        std::vector<std::pair<uint16_t, float>> most = sums;
        std::ranges::sort(most, std::greater{}, &std::pair<uint16_t, float>::second);
        VertexJoints joints;
        float total = 0.0f;
        for (size_t k = 0; k < std::min<size_t>(4, most.size()); ++k)
        {
            joints.Joints[k] = most[k].first;
            joints.Weights[static_cast<glm::length_t>(k)] = most[k].second;
            total += most[k].second;
        }
        if (total > 0.0f)
        {
            joints.Weights /= total;
        }
        return joints;
    }

    uint16_t GetHeaviest() const
    {
        const auto found = std::ranges::max_element(sums, {}, &std::pair<uint16_t, float>::second);
        return found != sums.end() ? found->first : 0;
    }

    bool IsEmpty() const
    {
        return sums.empty();
    }

private:
    std::vector<std::pair<uint16_t, float>> sums;
};

// The joints' matrices blended by a vertex's weights (as skinning moves it).
glm::mat4 Blend(const VertexJoints& carry, std::span<const glm::mat4> joints, size_t count)
{
    glm::mat4 blended(0.0f);
    float total = 0.0f;
    for (size_t k = 0; k < carry.Joints.size(); ++k)
    {
        const float weight = carry.Weights[static_cast<glm::length_t>(k)];
        if (weight > 0.0f && carry.Joints[k] < count)
        {
            blended += joints[carry.Joints[k]] * weight;
            total += weight;
        }
    }
    return total > 0.0f ? blended / total : glm::mat4(1.0f);
}

// The turn nearest `a` (as much of it as is a turn), found from `turn` (Müller et al. 2016).
glm::quat ExtractTurn(const glm::mat3& a, glm::quat turn, int32_t passes)
{
    for (int32_t pass = 0; pass < passes; ++pass)
    {
        const glm::mat3 r = glm::mat3_cast(turn);
        const glm::vec3 pull = glm::cross(r[0], a[0]) + glm::cross(r[1], a[1]) + glm::cross(r[2], a[2]);
        const float along = std::abs(glm::dot(r[0], a[0]) + glm::dot(r[1], a[1]) + glm::dot(r[2], a[2])) + 1.0e-9f;
        const glm::vec3 spin = pull / along;
        const float angle = glm::length(spin);
        if (angle < 1.0e-9f)
        {
            break;
        }
        turn = glm::normalize(glm::angleAxis(angle, spin / angle) * turn);
    }
    return turn;
}

// How far along each of two segments (0 to 1) their nearest points are (Ericson, "Real-Time Collision
// Detection", 5.1.9).
std::pair<float, float> GetNearest(glm::vec3 from1, glm::vec3 to1, glm::vec3 from2, glm::vec3 to2)
{
    const glm::vec3 along1 = to1 - from1;
    const glm::vec3 along2 = to2 - from2;
    const glm::vec3 apart = from1 - from2;
    const float length1 = glm::dot(along1, along1);
    const float length2 = glm::dot(along2, along2);
    const float f = glm::dot(along2, apart);
    if (length1 <= TINY && length2 <= TINY)
    {
        return {0.0f, 0.0f};
    }
    if (length1 <= TINY)
    {
        return {0.0f, std::clamp(f / length2, 0.0f, 1.0f)};
    }
    const float c = glm::dot(along1, apart);
    if (length2 <= TINY)
    {
        return {std::clamp(-c / length1, 0.0f, 1.0f), 0.0f};
    }
    const float b = glm::dot(along1, along2);
    const float denominator = (length1 * length2) - (b * b);
    float s = denominator > TINY ? std::clamp(((b * f) - (c * length2)) / denominator, 0.0f, 1.0f) : 0.0f;
    float t = ((b * s) + f) / length2;
    if (t < 0.0f)
    {
        t = 0.0f;
        s = std::clamp(-c / length1, 0.0f, 1.0f);
    }
    else if (t > 1.0f)
    {
        t = 1.0f;
        s = std::clamp((b - c) / length1, 0.0f, 1.0f);
    }
    return {s, t};
}

} // namespace

size_t MeshCloth::Build(const ModelData& model, const MeshData& mesh, std::span<const VertexJoints> skin,
                        std::span<const ClothPaint> paint, const ClothSettings& clothSettings, uint16_t firstJoint)
{
    settings = clothSettings;
    isStarted = false;
    pending = 0.0f;
    auto made = std::make_shared<Layout>();
    Layout& cloth = *made;
    cloth.FirstJoint = firstJoint;
    cloth.Skin.assign(skin.begin(), skin.end());
    layout = made;
    const size_t count = mesh.Vertices.size();
    if (skin.size() != count || firstJoint < model.Joints.size())
    {
        return 0;
    }

    // Each vertex's place (vertices split at a seam are one there), painted as the strongest of its
    // vertices (fixed, then moving), so a seam doesn't open.
    std::unordered_map<uint64_t, uint32_t> places;
    std::vector<uint32_t> placeOf(count);
    std::vector<glm::vec3> placeAt;
    std::vector<glm::vec3> placeFacing;
    std::vector<ClothPaint> placePaint;
    for (size_t v = 0; v < count; ++v)
    {
        const glm::vec3 at = mesh.Vertices[v].Position;
        const auto [found, isNew] =
            places.try_emplace(PackCell(glm::ivec3(glm::round(at / WELD))), static_cast<uint32_t>(placeAt.size()));
        if (isNew)
        {
            placeAt.push_back(at);
            placeFacing.emplace_back(0.0f);
            placePaint.push_back(ClothPaint::Ignore);
        }
        const uint32_t place = found->second;
        placeOf[v] = place;
        placeFacing[place] += mesh.Vertices[v].Normal;
        const ClothPaint painted = v < paint.size() ? paint[v] : ClothPaint::Ignore;
        if (GetHold(painted) > GetHold(placePaint[place]))
        {
            placePaint[place] = painted;
        }
    }

    // Places merged into points: each, in turn, with every place not yet merged painted as it is, as
    // close as the reduction.
    const float reduction = std::max(settings.Reduction, 0.0f);
    const float cellSize = std::max(reduction, 1.0e-3f);
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t place = 0; place < placeAt.size(); ++place)
    {
        if (placePaint[place] != ClothPaint::Ignore)
        {
            grid[PackCell(GetCell(placeAt[place], cellSize))].push_back(place);
        }
    }
    std::vector<int32_t> pointOf(placeAt.size(), -1);
    std::vector<std::vector<uint32_t>> members;
    for (uint32_t place = 0; place < placeAt.size(); ++place)
    {
        if (placePaint[place] == ClothPaint::Ignore || pointOf[place] >= 0)
        {
            continue;
        }
        const auto point = static_cast<int32_t>(members.size());
        members.emplace_back();
        const glm::ivec3 cell = GetCell(placeAt[place], cellSize);
        for (int32_t x = -1; x <= 1; ++x)
        {
            for (int32_t y = -1; y <= 1; ++y)
            {
                for (int32_t z = -1; z <= 1; ++z)
                {
                    const auto found = grid.find(PackCell(cell + glm::ivec3(x, y, z)));
                    if (found == grid.end())
                    {
                        continue;
                    }
                    for (const uint32_t other : found->second)
                    {
                        if (pointOf[other] < 0 && placePaint[other] == placePaint[place] &&
                            glm::distance(placeAt[other], placeAt[place]) <= reduction)
                        {
                            pointOf[other] = point;
                            members.back().push_back(other);
                        }
                    }
                }
            }
        }
    }

    // The points' edges and folds, from the triangles between them.
    const auto pointOfVertex = [&](uint32_t vertex) { return vertex < count ? pointOf[placeOf[vertex]] : -1; };
    std::unordered_set<uint64_t> edges;
    std::unordered_set<uint64_t> triangles;
    std::unordered_map<uint64_t, std::vector<uint32_t>> opposite;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
    {
        const std::array<int32_t, 3> corner{pointOfVertex(mesh.Indices[i]), pointOfVertex(mesh.Indices[i + 1]),
                                            pointOfVertex(mesh.Indices[i + 2])};
        if (corner[0] < 0 || corner[1] < 0 || corner[2] < 0)
        {
            continue;
        }
        std::array<uint32_t, 3> sorted{static_cast<uint32_t>(corner[0]), static_cast<uint32_t>(corner[1]),
                                       static_cast<uint32_t>(corner[2])};
        for (size_t k = 0; k < 3; ++k)
        {
            if (sorted[k] != sorted[(k + 1) % 3])
            {
                edges.insert(PackPair(sorted[k], sorted[(k + 1) % 3]));
            }
        }
        std::ranges::sort(sorted);
        if (sorted[0] == sorted[1] || sorted[1] == sorted[2])
        {
            continue;
        }
        const uint64_t key = (static_cast<uint64_t>(sorted[0]) << 42U) | (static_cast<uint64_t>(sorted[1]) << 21U) |
                             static_cast<uint64_t>(sorted[2]);
        if (triangles.insert(key).second)
        {
            opposite[PackPair(sorted[0], sorted[1])].push_back(sorted[2]);
            opposite[PackPair(sorted[1], sorted[2])].push_back(sorted[0]);
            opposite[PackPair(sorted[0], sorted[2])].push_back(sorted[1]);
        }
    }

    // In order, so the same mesh makes the same cloth everywhere.
    std::vector<uint64_t> edgeList(edges.begin(), edges.end());
    std::ranges::sort(edgeList);
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> foldList(opposite.begin(), opposite.end());
    std::ranges::sort(foldList, {}, &std::pair<uint64_t, std::vector<uint32_t>>::first);

    // Fixed points far from any moving one are no part of the cloth.
    const size_t gathered = members.size();
    std::vector<std::vector<uint32_t>> beside(gathered);
    for (const uint64_t edge : edgeList)
    {
        const auto a = static_cast<uint32_t>(edge >> 32U);
        const auto b = static_cast<uint32_t>(edge & 0xFFFFFFFFU);
        beside[a].push_back(b);
        beside[b].push_back(a);
    }
    const auto isFixed = [&](size_t point) { return placePaint[members[point][0]] == ClothPaint::Fixed; };
    std::vector<int32_t> hops(gathered, std::numeric_limits<int32_t>::max());
    std::queue<uint32_t> next;
    for (uint32_t point = 0; point < gathered; ++point)
    {
        if (!isFixed(point))
        {
            hops[point] = 0;
            next.push(point);
        }
    }
    if (next.empty())
    {
        return 0;
    }
    while (!next.empty())
    {
        const uint32_t point = next.front();
        next.pop();
        for (const uint32_t other : beside[point])
        {
            if (hops[other] > hops[point] + 1)
            {
                hops[other] = hops[point] + 1;
                next.push(other);
            }
        }
    }
    std::vector<int32_t> kept(gathered, -1);
    for (uint32_t point = 0; point < gathered; ++point)
    {
        if (hops[point] <= FIXED_RINGS)
        {
            kept[point] = static_cast<int32_t>(cloth.Rest.size());
            glm::vec3 sum(0.0f);
            glm::vec3 faces(0.0f);
            for (const uint32_t place : members[point])
            {
                sum += placeAt[place];
                const float length = glm::length(placeFacing[place]);
                faces += length > 1.0e-4f ? placeFacing[place] / length : glm::vec3(0.0f);
            }
            const auto share = static_cast<float>(members[point].size());
            cloth.Rest.push_back(sum / share);
            // Facing both ways (a two-sided sheet's sides merged), it faces neither.
            const float facingLength = glm::length(faces);
            cloth.Facing.push_back(facingLength > 0.5f * share ? faces / facingLength : glm::vec3(0.0f));
            cloth.Fixed.push_back(isFixed(point) ? 1 : 0);
        }
    }
    const size_t points = cloth.Rest.size();
    if (static_cast<size_t>(firstJoint) + points > std::numeric_limits<uint16_t>::max())
    {
        cloth.Rest.clear();
        return 0;
    }
    std::vector<JointSum> carries(points);
    for (size_t v = 0; v < count; ++v)
    {
        if (const int32_t point = pointOf[placeOf[v]]; point >= 0 && kept[static_cast<size_t>(point)] >= 0)
        {
            carries[static_cast<size_t>(kept[static_cast<size_t>(point)])].Add(skin[v]);
        }
    }
    JointSum anchor;
    for (size_t point = 0; point < points; ++point)
    {
        cloth.Carry.push_back(carries[point].GetMost());
        if (cloth.Fixed[point] != 0)
        {
            anchor.Add(cloth.Carry.back());
        }
    }
    cloth.Anchor = anchor.IsEmpty() ? -1 : anchor.GetHeaviest();

    std::vector<std::vector<uint32_t>> around(points);
    for (const uint64_t edge : edgeList)
    {
        const int32_t a = kept[static_cast<size_t>(edge >> 32U)];
        const int32_t b = kept[static_cast<size_t>(edge & 0xFFFFFFFFU)];
        if (a < 0 || b < 0)
        {
            continue;
        }
        const auto from = static_cast<uint32_t>(a);
        const auto to = static_cast<uint32_t>(b);
        cloth.Edges.push_back({from, to, glm::distance(cloth.Rest[from], cloth.Rest[to])});
        cloth.EdgePoints.push_back({from, to});
        around[from].push_back(to);
        around[to].push_back(from);
    }
    for (const auto& [edge, across] : foldList)
    {
        if (across.size() < 2)
        {
            continue;
        }
        const int32_t a = kept[across[0]];
        const int32_t b = kept[across[1]];
        if (a >= 0 && b >= 0 && a != b && !edges.contains(PackPair(across[0], across[1])))
        {
            const auto from = static_cast<uint32_t>(a);
            const auto to = static_cast<uint32_t>(b);
            cloth.Folds.push_back({from, to, glm::distance(cloth.Rest[from], cloth.Rest[to])});
        }
    }

    // How far along the cloth each point is from its fixed edge, by its edges (the nearest fixed point,
    // and the point before it from there).
    std::vector<float> distance(points, std::numeric_limits<float>::max());
    cloth.Before.assign(points, -1);
    cloth.Root.assign(points, -1);
    using Reached = std::pair<float, uint32_t>;
    std::priority_queue<Reached, std::vector<Reached>, std::greater<>> reach;
    for (uint32_t point = 0; point < points; ++point)
    {
        if (cloth.Fixed[point] != 0)
        {
            distance[point] = 0.0f;
            cloth.Root[point] = static_cast<int32_t>(point);
            reach.emplace(0.0f, point);
        }
    }
    while (!reach.empty())
    {
        const auto [gone, point] = reach.top();
        reach.pop();
        if (gone > distance[point])
        {
            continue;
        }
        for (const uint32_t other : around[point])
        {
            const float further = gone + glm::distance(cloth.Rest[point], cloth.Rest[other]);
            if (further < distance[other])
            {
                distance[other] = further;
                cloth.Before[other] = static_cast<int32_t>(point);
                cloth.Root[other] = cloth.Root[point];
                reach.emplace(further, other);
            }
        }
    }
    float farthest = 0.0f;
    for (size_t point = 0; point < points; ++point)
    {
        if (cloth.Fixed[point] == 0 && cloth.Root[point] >= 0)
        {
            farthest = std::max(farthest, distance[point]);
        }
    }
    cloth.Along.resize(points);
    cloth.RootLength.resize(points);
    for (size_t point = 0; point < points; ++point)
    {
        const bool isReached = cloth.Root[point] >= 0;
        cloth.Along[point] = cloth.Fixed[point] != 0        ? 0.0f
                             : isReached && farthest > 0.0f ? distance[point] / farthest
                                                            : 1.0f;
        cloth.RootLength[point] =
            isReached ? glm::distance(cloth.Rest[point], cloth.Rest[static_cast<size_t>(cloth.Root[point])]) : 0.0f;
        if (cloth.Fixed[point] == 0)
        {
            cloth.Order.push_back(static_cast<uint32_t>(point));
        }
    }
    std::ranges::sort(cloth.Order, {}, [&distance](uint32_t point) { return distance[point]; });

    // The points beside each, and the two most across from each other, to tell which way it faces.
    for (size_t point = 0; point < points; ++point)
    {
        cloth.BesideFrom.push_back(static_cast<uint32_t>(cloth.Beside.size()));
        cloth.Beside.insert(cloth.Beside.end(), around[point].begin(), around[point].end());
        std::array<int32_t, 2> across{-1, -1};
        float widest = 1.0e-10f;
        for (size_t i = 0; i < around[point].size(); ++i)
        {
            for (size_t j = i + 1; j < around[point].size(); ++j)
            {
                const float wide = glm::length(glm::cross(cloth.Rest[around[point][i]] - cloth.Rest[point],
                                                          cloth.Rest[around[point][j]] - cloth.Rest[point]));
                if (wide > widest)
                {
                    widest = wide;
                    across = {static_cast<int32_t>(around[point][i]), static_cast<int32_t>(around[point][j])};
                }
            }
        }
        cloth.Across.push_back(across);
    }
    cloth.BesideFrom.push_back(static_cast<uint32_t>(cloth.Beside.size()));

    // Each moving vertex drawn by the points nearest it: its own and those beside it, the nearer the
    // more, four at most.
    for (size_t v = 0; v < count; ++v)
    {
        const int32_t gatheredPoint = pointOf[placeOf[v]];
        if (placePaint[placeOf[v]] != ClothPaint::Move || gatheredPoint < 0)
        {
            continue;
        }
        const auto own = static_cast<uint32_t>(kept[static_cast<size_t>(gatheredPoint)]);
        const glm::vec3 at = mesh.Vertices[v].Position;
        std::vector<std::pair<uint32_t, float>> closest{{own, glm::distance(at, cloth.Rest[own])}};
        float reachOut = reduction;
        for (uint32_t i = cloth.BesideFrom[own]; i < cloth.BesideFrom[own + 1]; ++i)
        {
            const uint32_t other = cloth.Beside[i];
            closest.emplace_back(other, glm::distance(at, cloth.Rest[other]));
            reachOut = std::max(reachOut, glm::distance(cloth.Rest[own], cloth.Rest[other]));
        }
        reachOut *= 1.2f;
        for (auto& [point, weight] : closest)
        {
            const float falloff = std::max(0.0f, 1.0f - (weight / reachOut));
            weight = falloff * falloff;
        }
        std::ranges::sort(closest, std::greater{}, &std::pair<uint32_t, float>::second);
        VertexJoints bound;
        float total = 0.0f;
        for (size_t k = 0; k < std::min<size_t>(4, closest.size()); ++k)
        {
            bound.Joints[k] = static_cast<uint16_t>(firstJoint + closest[k].first);
            bound.Weights[static_cast<glm::length_t>(k)] = closest[k].second;
            total += closest[k].second;
        }
        if (total > 1.0e-6f)
        {
            bound.Weights /= total;
        }
        else
        {
            bound = {};
            bound.Joints[0] = static_cast<uint16_t>(firstJoint + own);
            bound.Weights[0] = 1.0f;
        }
        cloth.Skin[v] = bound;
    }
    return points;
}

std::span<const VertexJoints> MeshCloth::GetSkin() const
{
    return layout ? std::span<const VertexJoints>(layout->Skin) : std::span<const VertexJoints>();
}

std::span<const std::array<uint32_t, 2>> MeshCloth::GetEdges() const
{
    return layout ? std::span<const std::array<uint32_t, 2>>(layout->EdgePoints)
                  : std::span<const std::array<uint32_t, 2>>();
}

void MeshCloth::SetSettings(const ClothSettings& clothSettings)
{
    const float reduction = settings.Reduction;
    settings = clothSettings;
    settings.Reduction = reduction;
}

void MeshCloth::SetColliders(const ModelData& model, std::span<const ClothCollider> colliders)
{
    body.assign(colliders.begin(), colliders.end());
    std::vector<glm::mat4> rest;
    PoseNodes(model, nullptr, 0.0f, rest);
    fromRest.clear();
    for (const ClothCollider& collider : body)
    {
        const bool isCarried = collider.Node >= 0 && static_cast<size_t>(collider.Node) < rest.size();
        fromRest.push_back(isCarried ? glm::affineInverse(rest[static_cast<size_t>(collider.Node)]) : glm::mat4(1.0f));
    }
}

void MeshCloth::SetSceneColliders(std::span<const ClothCollider> colliders)
{
    scene.assign(colliders.begin(), colliders.end());
}

void MeshCloth::Reset()
{
    isStarted = false;
    pending = 0.0f;
}

void MeshCloth::Step(std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints, float seconds)
{
    if (!layout || layout->Rest.empty() || joints.size() < layout->FirstJoint)
    {
        return;
    }
    const Layout& cloth = *layout;
    const size_t count = cloth.Rest.size();
    joints.resize(std::max(joints.size(), static_cast<size_t>(cloth.FirstJoint) + count));
    Animate(joints);
    PlaceColliders(nodes);
    const glm::mat4 anchor = cloth.Anchor >= 0 && static_cast<size_t>(cloth.Anchor) < cloth.FirstJoint
                                 ? joints[static_cast<size_t>(cloth.Anchor)]
                                 : glm::mat4(1.0f);
    bool isTurnStarting = false;
    if (isStarted)
    {
        // The body's move since the last frame: a jump starts again; the rest carries the cloth along,
        // as much as it doesn't lag behind.
        const glm::mat4 moved = anchor * glm::affineInverse(anchorWas);
        const float turned = (glm::mat3(moved)[0][0] + glm::mat3(moved)[1][1] + glm::mat3(moved)[2][2] - 1.0f) * 0.5f;
        if (glm::distance(glm::vec3(anchor[3]), glm::vec3(anchorWas[3])) > JUMP || turned < JUMP_TURN)
        {
            isStarted = false;
        }
        else
        {
            const float carry = 1.0f - std::clamp(settings.Inertia, 0.0f, 1.0f);
            for (size_t point = 0; point < count; ++point)
            {
                for (glm::vec3* at : {&position[point], &previous[point], &last[point]})
                {
                    *at += (glm::vec3(moved * glm::vec4(*at, 1.0f)) - *at) * carry;
                }
            }
        }
    }
    if (!isStarted)
    {
        position = animated;
        previous = animated;
        last = animated;
        touched.assign(count, glm::vec3(0.0f));
        turns.assign(count, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        Settle();
        pending = 0.0f;
        isStarted = true;
        isTurnStarting = true;
    }
    anchorWas = anchor;
    pending = std::min(pending + std::max(seconds, 0.0f), STEP_SECONDS * static_cast<float>(MAX_STEPS));
    while (pending >= STEP_SECONDS)
    {
        pending -= STEP_SECONDS;
        last = position;
        StepOnce();
    }
    // Shown between its last two steps, as far as the time since the last is toward the next, so it
    // moves every frame at any frame rate; fixed points where the animation puts them.
    const float between = pending / STEP_SECONDS;
    shown.resize(count);
    for (size_t point = 0; point < count; ++point)
    {
        shown[point] = cloth.Fixed[point] != 0 ? animated[point] : glm::mix(last[point], position[point], between);
    }
    if (isTurnStarting)
    {
        for (size_t point = 0; point < count; ++point)
        {
            turns[point] = ExtractTurn(glm::mat3(carried[point]), turns[point], START_TURN_PASSES);
        }
    }
    WriteJoints(joints);
}

void MeshCloth::Animate(std::span<const glm::mat4> joints)
{
    const Layout& cloth = *layout;
    const size_t count = cloth.Rest.size();
    animated.resize(count);
    facing.resize(count);
    carried.resize(count);
    for (size_t point = 0; point < count; ++point)
    {
        carried[point] = Blend(cloth.Carry[point], joints, cloth.FirstJoint);
        animated[point] = glm::vec3(carried[point] * glm::vec4(cloth.Rest[point], 1.0f));
        const glm::vec3 way = glm::mat3(carried[point]) * cloth.Facing[point];
        const float length = glm::length(way);
        facing[point] = length > 1.0e-6f ? way / length : glm::vec3(0.0f);
    }
}

void MeshCloth::PlaceColliders(std::span<const glm::mat4> nodes)
{
    placed.clear();
    const auto place = [this](const ClothCollider& collider, const glm::mat4& carry)
    {
        Placed where;
        where.Shape = collider.Shape;
        if (collider.Shape == ColliderShape::Box)
        {
            where.Space = carry * glm::translate(glm::mat4(1.0f), collider.From);
            where.FromSpace = glm::affineInverse(where.Space);
            where.Half = glm::max(collider.Half, glm::vec3(0.0f));
        }
        else
        {
            where.From = glm::vec3(carry * glm::vec4(collider.From, 1.0f));
            where.To =
                collider.Shape == ColliderShape::Ball ? where.From : glm::vec3(carry * glm::vec4(collider.To, 1.0f));
            where.FromRadius = std::max(collider.FromRadius, 0.0f);
            where.ToRadius =
                collider.Shape == ColliderShape::Ball ? where.FromRadius : std::max(collider.ToRadius, 0.0f);
        }
        placed.push_back(where);
    };
    for (size_t i = 0; i < body.size(); ++i)
    {
        const int32_t node = body[i].Node;
        const bool isCarried = node >= 0 && static_cast<size_t>(node) < nodes.size();
        place(body[i], isCarried ? nodes[static_cast<size_t>(node)] * fromRest[i] : glm::mat4(1.0f));
    }
    for (const ClothCollider& collider : scene)
    {
        place(collider, glm::mat4(1.0f));
    }
}

void MeshCloth::Settle()
{
    for (int32_t step = 0; step < SETTLE_STEPS; ++step)
    {
        StepOnce();
    }
    previous = position;
    last = position;
}

void MeshCloth::StepOnce()
{
    const Layout& cloth = *layout;
    const size_t count = cloth.Rest.size();
    const float keep = 1.0f - std::clamp(settings.Damping, 0.0f, 1.0f);
    const float mostMove = MOST_SPEED * STEP_SECONDS;
    const glm::vec3 fall = DOWN * (settings.Gravity * STEP_SECONDS * STEP_SECONDS);
    const float slide = 1.0f - std::clamp(settings.Friction, 0.0f, 1.0f);
    for (size_t point = 0; point < count; ++point)
    {
        if (cloth.Fixed[point] != 0)
        {
            previous[point] = animated[point];
            position[point] = animated[point];
            continue;
        }
        glm::vec3 move = (position[point] - previous[point]) * keep;
        // Touching something, it doesn't bounce off it (going on out of it at the speed it was pushed
        // out), and slides on along it only as far as it doesn't stick.
        if (const glm::vec3 out = touched[point]; out != glm::vec3(0.0f))
        {
            const float outward = glm::dot(move, out);
            move = (out * std::min(outward, 0.0f)) + ((move - (out * outward)) * slide);
        }
        const float moveLength = glm::length(move);
        if (moveLength > mostMove)
        {
            move *= mostMove / moveLength;
        }
        previous[point] = position[point];
        position[point] += move + fall;
        touched[point] = glm::vec3(0.0f);
    }
    const float stretch = PerPass(settings.Stretch, SOLVE_PASSES);
    const float squeeze = PerPass(settings.Squeeze, SOLVE_PASSES);
    const float bend = PerPass(settings.Bending, SOLVE_PASSES);
    shaping.resize(count);
    for (size_t point = 0; point < count; ++point)
    {
        shaping[point] = PerPass(glm::mix(settings.Shape.x, settings.Shape.y, cloth.Along[point]), SOLVE_PASSES);
    }
    for (int32_t pass = 0; pass < SOLVE_PASSES; ++pass)
    {
        KeepLengths(cloth.Edges, stretch, squeeze);
        KeepLengths(cloth.Folds, bend, bend);
        // Turned back toward the way the animation has it from the point before it, from the fixed
        // edge out.
        for (const uint32_t point : cloth.Order)
        {
            const int32_t before = cloth.Before[point];
            if (before < 0)
            {
                continue;
            }
            const auto from = static_cast<size_t>(before);
            const glm::vec3 want = animated[point] - animated[from];
            const float wantLength = glm::length(want);
            if (wantLength < 1.0e-6f)
            {
                continue;
            }
            const glm::vec3 goal =
                position[from] + (want * (glm::distance(position[point], position[from]) / wantLength));
            position[point] += (goal - position[point]) * shaping[point];
        }
        for (const uint32_t point : cloth.Order)
        {
            // No farther from its fixed point than it can stretch to.
            if (const int32_t root = cloth.Root[point]; root >= 0)
            {
                const glm::vec3 out = position[point] - position[static_cast<size_t>(root)];
                const float most = cloth.RootLength[point] * TETHER;
                const float length = glm::length(out);
                if (length > most && length > 1.0e-6f)
                {
                    position[point] = position[static_cast<size_t>(root)] + (out * (most / length));
                }
            }
            // Within reach of where the animation puts it, and not behind it.
            const float reach = std::max(glm::mix(settings.Reach.x, settings.Reach.y, cloth.Along[point]), 0.0f);
            const glm::vec3 away = position[point] - animated[point];
            const float awayLength = glm::length(away);
            if (awayLength > reach)
            {
                position[point] = animated[point] + (away * (reach / awayLength));
            }
            if (settings.Backstop >= 0.0f && facing[point] != glm::vec3(0.0f))
            {
                const float behind = glm::dot(position[point] - animated[point], facing[point]);
                if (behind < -settings.Backstop)
                {
                    position[point] += facing[point] * (-settings.Backstop - behind);
                }
            }
        }
        Collide(pass >= SOLVE_PASSES - EDGE_PASSES);
    }
    for (const uint32_t point : cloth.Order)
    {
        const glm::vec3 away = position[point] - animated[point];
        const float most =
            std::max(glm::mix(settings.Reach.x, settings.Reach.y, cloth.Along[point]), 0.0f) + PAST_REACH;
        const float awayLength = glm::length(away);
        if (!std::isfinite(awayLength))
        {
            position[point] = animated[point];
            previous[point] = animated[point];
        }
        else if (awayLength > most)
        {
            position[point] = animated[point] + (away * (most / awayLength));
        }
    }
}

void MeshCloth::KeepLengths(std::span<const Link> links, float stretch, float squeeze)
{
    const Layout& cloth = *layout;
    for (const Link& link : links)
    {
        const float a = cloth.Fixed[link.A] != 0 ? 0.0f : 1.0f;
        const float b = cloth.Fixed[link.B] != 0 ? 0.0f : 1.0f;
        if (a + b <= 0.0f)
        {
            continue;
        }
        const glm::vec3 apart = position[link.B] - position[link.A];
        const float length = glm::length(apart);
        if (length < 1.0e-7f)
        {
            continue;
        }
        const float stiffness = length > link.Length ? stretch : squeeze;
        const glm::vec3 fix = apart * (((length - link.Length) / length) * stiffness / (a + b));
        position[link.A] += fix * a;
        position[link.B] -= fix * b;
    }
}

glm::vec3 MeshCloth::PushOut(const Placed& collider, glm::vec3 at, glm::vec3 from, float radius,
                             glm::vec3& normal) const
{
    if (collider.Shape == ColliderShape::Box)
    {
        const glm::vec3 local = glm::vec3(collider.FromSpace * glm::vec4(at, 1.0f));
        const glm::vec3 was = glm::vec3(collider.FromSpace * glm::vec4(from, 1.0f));
        const glm::vec3 half = collider.Half + glm::vec3(radius);
        if (std::abs(local.x) >= half.x || std::abs(local.y) >= half.y || std::abs(local.z) >= half.z)
        {
            return at;
        }
        // Out the face it came in by (the last it crossed from where it was a step before), else the
        // nearest.
        glm::length_t axis = 0;
        float side = 1.0f;
        float latest = -1.0f;
        for (glm::length_t k = 0; k < 3; ++k)
        {
            if (std::abs(was[k]) >= half[k] && std::abs(was[k] - local[k]) > 1.0e-7f)
            {
                const float wasSide = was[k] > 0.0f ? 1.0f : -1.0f;
                const float crossed = ((wasSide * half[k]) - was[k]) / (local[k] - was[k]);
                if (crossed > latest)
                {
                    latest = crossed;
                    axis = k;
                    side = wasSide;
                }
            }
        }
        if (latest < 0.0f)
        {
            float nearest = std::numeric_limits<float>::max();
            for (glm::length_t k = 0; k < 3; ++k)
            {
                const float depth = half[k] - std::abs(local[k]);
                if (depth < nearest)
                {
                    nearest = depth;
                    axis = k;
                    side = local[k] >= 0.0f ? 1.0f : -1.0f;
                }
            }
        }
        glm::vec3 out = local;
        out[axis] = side * half[axis];
        glm::vec3 way(0.0f);
        way[axis] = side;
        normal = glm::normalize(glm::mat3(collider.Space) * way);
        return glm::vec3(collider.Space * glm::vec4(out, 1.0f));
    }
    const glm::vec3 along = collider.To - collider.From;
    const float lengthSquared = glm::dot(along, along);
    const auto nearestTo = [&collider, &along, lengthSquared](glm::vec3 point)
    {
        return lengthSquared > 1.0e-12f ? std::clamp(glm::dot(point - collider.From, along) / lengthSquared, 0.0f, 1.0f)
                                        : 0.0f;
    };
    const float share = nearestTo(at);
    const glm::vec3 nearest = collider.From + (along * share);
    const float thick = glm::mix(collider.FromRadius, collider.ToRadius, share) + radius;
    glm::vec3 away = at - nearest;
    const float distance = glm::length(away);
    if (distance >= thick)
    {
        return at;
    }
    // Out on the side it was on a step before (deep in, the nearest way out flips from side to side as
    // it moves a little: the cloth would jump across the collider and back every step).
    const glm::vec3 wasAway = from - (collider.From + (along * nearestTo(from)));
    if (distance < 1.0e-6f || glm::dot(away, wasAway) < 0.0f)
    {
        away = wasAway;
        if (glm::length(away) < 1.0e-6f)
        {
            away = -DOWN;
        }
    }
    normal = glm::normalize(away);
    return nearest + (normal * thick);
}

void MeshCloth::Collide(bool isEdges)
{
    const Layout& cloth = *layout;
    const size_t count = cloth.Rest.size();
    const float radius = std::max(settings.Radius, 0.0f);
    for (size_t point = 0; point < count; ++point)
    {
        if (cloth.Fixed[point] != 0)
        {
            continue;
        }
        for (const Placed& collider : placed)
        {
            glm::vec3 normal(0.0f);
            const glm::vec3 out = PushOut(collider, position[point], previous[point], radius, normal);
            if (out != position[point])
            {
                position[point] = out;
                touched[point] = normal;
            }
        }
        if (floor && position[point].y < *floor + radius)
        {
            position[point].y = *floor + radius;
            touched[point] = -DOWN;
        }
    }
    if (!isEdges || !settings.Edges)
    {
        return;
    }
    // Each edge out of the capsules and balls thin enough to pass between its ends (a thicker one meets
    // its points first): its nearest point to one pushed out, its ends moved as far as they bear it.
    for (const Placed& collider : placed)
    {
        if (collider.Shape == ColliderShape::Box)
        {
            continue;
        }
        const glm::vec3 middle = (collider.From + collider.To) * 0.5f;
        const float bound = (glm::distance(collider.From, collider.To) * 0.5f) +
                            std::max(collider.FromRadius, collider.ToRadius) + radius;
        for (const Link& edge : cloth.Edges)
        {
            const float a = cloth.Fixed[edge.A] != 0 ? 0.0f : 1.0f;
            const float b = cloth.Fixed[edge.B] != 0 ? 0.0f : 1.0f;
            if (a + b <= 0.0f)
            {
                continue;
            }
            const glm::vec3 from = position[edge.A];
            const glm::vec3 to = position[edge.B];
            const float thickest = std::max(collider.FromRadius, collider.ToRadius) + radius;
            if (thickest * 2.0f >= edge.Length ||
                glm::distance((from + to) * 0.5f, middle) > bound + (glm::distance(from, to) * 0.5f))
            {
                continue;
            }
            const auto [s, t] = GetNearest(from, to, collider.From, collider.To);
            // Touching at an end, it's that point's (kept out on its own), not the edge's: an end moved
            // for a touch at the other would be flung.
            if (s < EDGE_ENDS || s > 1.0f - EDGE_ENDS)
            {
                continue;
            }
            const glm::vec3 away = glm::mix(from, to, s) - glm::mix(collider.From, collider.To, t);
            const float distance = glm::length(away);
            const float thick = glm::mix(collider.FromRadius, collider.ToRadius, t) + radius;
            if (distance >= thick || distance < 1.0e-6f)
            {
                continue;
            }
            // Gone through it since a step before (its sides swapped), it isn't pushed on through.
            const auto [wasS, wasT] = GetNearest(previous[edge.A], previous[edge.B], collider.From, collider.To);
            const glm::vec3 wasAway =
                glm::mix(previous[edge.A], previous[edge.B], wasS) - glm::mix(collider.From, collider.To, wasT);
            if (glm::dot(away, wasAway) < 0.0f)
            {
                continue;
            }
            const float bearA = a * (1.0f - s);
            const float bearB = b * s;
            const float bearing = (bearA * (1.0f - s)) + (bearB * s);
            if (bearing < 1.0e-6f)
            {
                continue;
            }
            const glm::vec3 normal = away / distance;
            const float depth = thick - distance;
            const float most = depth * MOST_EDGE_PUSH;
            position[edge.A] += normal * std::min(depth * bearA / bearing, most);
            position[edge.B] += normal * std::min(depth * bearB / bearing, most);
            if (bearA > 0.0f)
            {
                touched[edge.A] = normal;
            }
            if (bearB > 0.0f)
            {
                touched[edge.B] = normal;
            }
        }
    }
}

void MeshCloth::WriteJoints(std::vector<glm::mat4>& joints)
{
    const Layout& cloth = *layout;
    const size_t count = cloth.Rest.size();
    for (size_t point = 0; point < count; ++point)
    {
        glm::mat4& joint = joints[cloth.FirstJoint + point];
        const uint32_t besideFrom = cloth.BesideFrom[point];
        const uint32_t besideTo = cloth.BesideFrom[point + 1];
        if (cloth.Fixed[point] != 0 || besideFrom == besideTo)
        {
            // Fixed, as its joints move it; alone, as they turn it, where it is.
            joint = glm::translate(glm::mat4(1.0f), shown[point] - animated[point]) * carried[point];
            continue;
        }
        // Turned as the points around it are from how they were at rest (the turn nearest theirs), and
        // as the way it faces is.
        const glm::vec3 here = shown[point];
        const glm::vec3 rest = cloth.Rest[point];
        glm::mat3 spread(0.0f);
        float lengths = 0.0f;
        for (uint32_t i = besideFrom; i < besideTo; ++i)
        {
            const uint32_t other = cloth.Beside[i];
            const glm::vec3 restOut = cloth.Rest[other] - rest;
            spread += glm::outerProduct(shown[other] - here, restOut);
            lengths += glm::dot(restOut, restOut);
        }
        if (const auto [i, j] = cloth.Across[point]; i >= 0 && j >= 0)
        {
            const glm::vec3 restFacing =
                glm::cross(cloth.Rest[static_cast<size_t>(i)] - rest, cloth.Rest[static_cast<size_t>(j)] - rest);
            const glm::vec3 nowFacing =
                glm::cross(shown[static_cast<size_t>(i)] - here, shown[static_cast<size_t>(j)] - here);
            const float restLength = glm::length(restFacing);
            const float nowLength = glm::length(nowFacing);
            if (restLength > 1.0e-10f && nowLength > 1.0e-10f)
            {
                const float weight = lengths / static_cast<float>(besideTo - besideFrom);
                spread += glm::outerProduct(nowFacing / nowLength, restFacing / restLength) * weight;
            }
        }
        turns[point] = ExtractTurn(spread, turns[point], TURN_PASSES);
        joint = glm::translate(glm::mat4(1.0f), here) * glm::mat4_cast(turns[point]) *
                glm::translate(glm::mat4(1.0f), -rest);
    }
}

} // namespace hyoshi::renderer
