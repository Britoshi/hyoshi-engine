#pragma once

#include "renderer/Mesh.h"
#include "renderer/Model.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hyoshi::renderer
{

// What a vertex of a cloth's mesh does, as it's painted (Magica Cloth's way): moves with the cloth,
// is fixed (where its joints put it, the cloth hanging from it), or is no part of the cloth (moved
// by its joints alone).
enum class ClothPaint : uint8_t
{
    Move,
    Fixed,
    Ignore
};

enum class ColliderShape : uint8_t
{
    Ball,
    Capsule,
    Box
};

// Something a cloth keeps out of (a body's limbs, as a ragdoll's are; a seat): where it is with the
// model at rest (PoseNodes's, with no animation), carried by `Node` as it moves from there (-1: it
// stays). A ball at `From`, `FromRadius` thick; a capsule from `From` to `To`, `FromRadius` thick at
// one end and `ToRadius` at the other; or a box, its middle at `From`, `Half` its size each way along
// the model's axes.
struct ClothCollider
{
    ColliderShape Shape = ColliderShape::Capsule;
    int32_t Node = -1;
    glm::vec3 From{0.0f};
    glm::vec3 To{0.0f};
    float FromRadius = 0.05f;
    float ToRadius = 0.05f;
    glm::vec3 Half{0.1f};

    bool operator==(const ClothCollider&) const = default;
};

// How a cloth moves. A pair is at its fixed edge, then at its farthest from it (in between, as far
// along as a point is).
struct ClothSettings
{
    // Vertices this close (meters) are one point of the cloth: fewer points are cheaper and coarser.
    float Reduction = 0.02f;
    // Down (meters a second a second), and the share of its speed a point loses each step.
    float Gravity = 9.8f;
    float Damping = 0.03f;
    // How hard the cloth keeps its length (0 to 1), stretched and squeezed: cloth gives way most to
    // being squeezed, buckling.
    float Stretch = 1.0f;
    float Squeeze = 0.4f;
    // How hard it keeps its folds as they're modeled (0 to 1).
    float Bending = 0.5f;
    // How hard each point is turned back toward the way the animation has it from the point before
    // it (0 to 1): what keeps a skirt's flare, or a coat's hang, as modeled.
    glm::vec2 Shape{0.3f, 0.05f};
    // How far a point may go from where the animation puts it (meters), and how far behind it, into
    // the body (against the way its surface faces there; below zero, as far as it likes).
    glm::vec2 Reach{0.02f, 0.2f};
    float Backstop = -1.0f;
    // How thick the cloth is (meters), kept that far out of colliders; how much it sticks to what it
    // touches instead of sliding on (0 to 1).
    float Radius = 0.01f;
    float Friction = 0.3f;
    // Its edges kept out of colliders thin enough to pass between two of its points, as well as its
    // points (Magica Cloth's edge collision): for thin limbs and a coarse cloth; a little dearer.
    bool Edges = false;
    // How far the cloth lags behind the body as it moves (0: carried along with it; 1: left where it
    // was, all of the body's moves swinging it).
    float Inertia = 0.6f;

    bool operator==(const ClothSettings&) const = default;
};

// A skinned mesh moving as cloth (Magica Cloth's mesh cloth): its vertices merged into fewer points
// (those painted fixed held where the animation puts them, those painted to move swung under
// gravity), each kept at its length to the points beside it, in its folds, toward the shape the
// animation gives it, within reach of where the animation puts it, and out of colliders and above a
// floor; all of it in steps of a fixed length (90 a second), so the same motion moves it the same way
// every time. The mesh is drawn skinned to its points: each moving point is a joint (after the
// model's), turned as the points around it turn, and its vertices move with the points nearest them.
class MeshCloth
{
public:
    // Builds the cloth from a skinned mesh as it was bound (`mesh`'s vertices, `skin` their joints in
    // `model`, as ModelPart has them) and how each vertex is painted (one each; a vertex past the
    // last is ignored). Its points are joints from `firstJoint` on. Returns how many points it has:
    // none if no vertex moves (it then draws as it did).
    size_t Build(const ModelData& model, const MeshData& mesh, std::span<const VertexJoints> skin,
                 std::span<const ClothPaint> paint, const ClothSettings& settings, uint16_t firstJoint);

    // The mesh's vertices' joints to draw it by: those that move, bound to the cloth's points; the
    // rest as they were.
    std::span<const VertexJoints> GetSkin() const;

    // How it moves (all but Reduction, which needs a Build).
    void SetSettings(const ClothSettings& settings);
    // What it keeps out of: the body's colliders, as `model` is at rest.
    void SetColliders(const ModelData& model, std::span<const ClothCollider> colliders);
    // Others besides, until set again (a seat): in the model's space as it stands (Node unused).
    void SetSceneColliders(std::span<const ClothCollider> colliders);
    void SetFloor(std::optional<float> height)
    {
        floor = height;
    }

    // The next Step starts again from the animation as it stands, the cloth first left to settle.
    void Reset();

    // Moves the cloth on by `seconds` since the last step (at most a few steps' worth), the body as
    // `nodes` (PoseNodes's, as moved) and `joints` (PoseJoints's from them) have it, and puts its
    // points' joints into `joints` after the model's (resizing it to fit).
    void Step(std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints, float seconds);

    size_t GetCount() const
    {
        return layout ? layout->Rest.size() : 0;
    }
    // For an editor: each point as it was last shown (the model's space), whether it's fixed, and the
    // points beside each other.
    std::span<const glm::vec3> GetPoints() const
    {
        return shown;
    }
    bool IsFixed(size_t point) const
    {
        return layout && layout->Fixed[point] != 0;
    }
    std::span<const std::array<uint32_t, 2>> GetEdges() const;

private:
    // Two points kept at a length (the cloth's edges, and across its folds).
    struct Link
    {
        uint32_t A = 0;
        uint32_t B = 0;
        float Length = 0.0f;
    };

    // What a Build makes, shared by copies (each with its own motion).
    struct Layout
    {
        uint16_t FirstJoint = 0;
        // Each point at rest (in the mesh's space as bound), the way its surface faces there (zero if
        // it faces both ways), the joints that move it as the animation has it, and whether it's
        // fixed.
        std::vector<glm::vec3> Rest;
        std::vector<glm::vec3> Facing;
        std::vector<VertexJoints> Carry;
        std::vector<uint8_t> Fixed;
        // How far along the cloth a point is from its fixed edge (0 to 1), the point before it
        // toward the edge, and the fixed point nearest it and how far it is from it at rest (-1 and
        // 0 for a point with none).
        std::vector<float> Along;
        std::vector<int32_t> Before;
        std::vector<int32_t> Root;
        std::vector<float> RootLength;
        // The moving points, nearest the fixed edge first.
        std::vector<uint32_t> Order;
        // The points beside each (offsets into Beside), and two of them, to tell which way it faces.
        std::vector<uint32_t> BesideFrom;
        std::vector<uint32_t> Beside;
        std::vector<std::array<int32_t, 2>> Across;
        std::vector<Link> Edges;
        std::vector<Link> Folds;
        std::vector<std::array<uint32_t, 2>> EdgePoints;
        // The joint the cloth hangs from most (its fixed points'), whose moves it lags behind.
        int32_t Anchor = -1;
        std::vector<VertexJoints> Skin;
    };

    // A collider where it is this step: a capsule (a ball's ends together), or a box in a space.
    struct Placed
    {
        ColliderShape Shape = ColliderShape::Capsule;
        glm::vec3 From{0.0f};
        glm::vec3 To{0.0f};
        float FromRadius = 0.0f;
        float ToRadius = 0.0f;
        glm::vec3 Half{0.0f};
        glm::mat4 Space{1.0f};
        glm::mat4 FromSpace{1.0f};
    };

    void Animate(std::span<const glm::mat4> joints);
    void PlaceColliders(std::span<const glm::mat4> nodes);
    void Settle();
    void StepOnce();
    void KeepLengths(std::span<const Link> links, float stretch, float squeeze);
    void Collide(bool isEdges);
    // `at` (a step before at `from`) out of a collider, as thick as `radius`; `normal` the way out.
    glm::vec3 PushOut(const Placed& collider, glm::vec3 at, glm::vec3 from, float radius, glm::vec3& normal) const;
    void WriteJoints(std::vector<glm::mat4>& joints);

    std::shared_ptr<const Layout> layout;
    ClothSettings settings;
    std::vector<ClothCollider> body;
    std::vector<glm::mat4> fromRest;
    std::vector<ClothCollider> scene;
    std::vector<Placed> placed;
    std::optional<float> floor;
    // Each point now, a step before, and before this frame's steps (shown between the last two); its
    // joints blended as the animation moves it, where that puts it, and which way it faces there; how
    // hard it's turned back to that shape each pass; what it last touched (the way out of it, zero if
    // nothing); as last shown, and each moving point's turn from rest then.
    std::vector<glm::vec3> position;
    std::vector<glm::vec3> previous;
    std::vector<glm::vec3> last;
    std::vector<glm::mat4> carried;
    std::vector<glm::vec3> animated;
    std::vector<glm::vec3> facing;
    std::vector<float> shaping;
    std::vector<glm::vec3> touched;
    std::vector<glm::vec3> shown;
    std::vector<glm::quat> turns;
    glm::mat4 anchorWas{1.0f};
    float pending = 0.0f;
    bool isStarted = false;
};

} // namespace hyoshi::renderer
