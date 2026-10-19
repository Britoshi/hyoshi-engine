#include "renderer/MeshCloth.h"
#include "renderer/Model.h"
#include "renderer/Skinning.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace
{

using hyoshi::renderer::ClothPaint;

constexpr float SPACING = 0.1f;
constexpr uint32_t ACROSS = 6;
constexpr uint32_t ALONG = 5;

// A sheet ACROSS by ALONG points, SPACING apart, level, out along +z from its edge along x at the
// origin, facing up; skinned to the one joint of a model with one node ("Root"), its first row painted
// fixed and the rest to move.
struct Sheet
{
    hyoshi::renderer::ModelData Model;
    hyoshi::renderer::MeshData Mesh;
    std::vector<hyoshi::renderer::VertexJoints> Skin;
    std::vector<ClothPaint> Paint;
};

Sheet MakeSheet()
{
    Sheet sheet;
    sheet.Model.Nodes = {{"Root", -1}};
    sheet.Model.Joints = {{0, glm::mat4(1.0f)}};
    for (uint32_t row = 0; row < ALONG; ++row)
    {
        for (uint32_t column = 0; column < ACROSS; ++column)
        {
            hyoshi::renderer::MeshVertex vertex;
            vertex.Position = {static_cast<float>(column) * SPACING, 0.0f, static_cast<float>(row) * SPACING};
            vertex.Normal = {0.0f, 1.0f, 0.0f};
            sheet.Mesh.Vertices.push_back(vertex);
            hyoshi::renderer::VertexJoints joints;
            joints.Weights = {1.0f, 0.0f, 0.0f, 0.0f};
            sheet.Skin.push_back(joints);
            sheet.Paint.push_back(row == 0 ? ClothPaint::Fixed : ClothPaint::Move);
        }
    }
    for (uint32_t row = 0; row + 1 < ALONG; ++row)
    {
        for (uint32_t column = 0; column + 1 < ACROSS; ++column)
        {
            const uint32_t at = (row * ACROSS) + column;
            sheet.Mesh.Indices.insert(sheet.Mesh.Indices.end(),
                                      {at, at + ACROSS, at + 1, at + 1, at + ACROSS, at + ACROSS + 1});
        }
    }
    return sheet;
}

// Each point merged from one vertex; let fall, unless held to its shape.
hyoshi::renderer::ClothSettings Loose()
{
    hyoshi::renderer::ClothSettings settings;
    settings.Reduction = 0.01f;
    settings.Shape = {0.0f, 0.0f};
    settings.Reach = {2.0f, 2.0f};
    settings.Inertia = 1.0f;
    return settings;
}

// The sheet's vertices as the cloth draws them after `frames` frames at 60 a second, the root as
// `place` has it each frame (by its frame).
template <typename Place>
std::vector<hyoshi::renderer::MeshVertex> Run(Sheet& sheet, hyoshi::renderer::MeshCloth& cloth, int frames, Place place)
{
    std::vector<glm::mat4> nodes;
    std::vector<glm::mat4> joints;
    for (int frame = 0; frame < frames; ++frame)
    {
        place(sheet.Model.Nodes[0], frame);
        hyoshi::renderer::PoseNodes(sheet.Model, nullptr, 0.0f, nodes);
        hyoshi::renderer::PoseJoints(sheet.Model, nodes, joints);
        cloth.Step(nodes, joints, 1.0f / 60.0f);
    }
    std::vector<hyoshi::renderer::MeshVertex> drawn(sheet.Mesh.Vertices.size());
    hyoshi::renderer::SkinVertices(sheet.Mesh.Vertices, cloth.GetSkin(), joints, drawn);
    return drawn;
}

void Still(hyoshi::renderer::ModelNode& /*root*/, int /*frame*/)
{
}

} // namespace

TEST_CASE("MeshCloth merges a mesh's vertices into points and binds those that move to them")
{
    Sheet sheet = MakeSheet();
    hyoshi::renderer::MeshCloth cloth;
    REQUIRE(cloth.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, Loose(), 1) == ACROSS * ALONG);
    REQUIRE(cloth.GetSkin().size() == sheet.Mesh.Vertices.size());
    for (size_t v = 0; v < sheet.Mesh.Vertices.size(); ++v)
    {
        // Fixed, as it was; moving, by the cloth's points (joints after the model's one).
        const bool isMoving = sheet.Paint[v] == ClothPaint::Move;
        CHECK((cloth.GetSkin()[v].Joints[0] >= 1) == isMoving);
    }
    CHECK(cloth.GetEdges().size() > ACROSS * ALONG);

    // Points twice as far apart as the reduction: a quarter as many (and the fixed row two rows from
    // the rest at most, so all of it stays).
    hyoshi::renderer::ClothSettings coarse = Loose();
    coarse.Reduction = SPACING * 1.5f;
    hyoshi::renderer::MeshCloth merged;
    const size_t points = merged.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, coarse, 1);
    CHECK(points < ACROSS * ALONG / 2);
    CHECK(points > 0);

    // Nothing painted to move: no cloth, drawn as it was.
    std::vector<ClothPaint> fixed(sheet.Paint.size(), ClothPaint::Fixed);
    hyoshi::renderer::MeshCloth none;
    CHECK(none.Build(sheet.Model, sheet.Mesh, sheet.Skin, fixed, Loose(), 1) == 0);
    CHECK(none.GetSkin()[ACROSS * 2].Joints[0] == 0);
}

TEST_CASE("MeshCloth draws its mesh where it is at rest, and as its body carries it")
{
    // Without gravity, held to its shape, the cloth stays as modeled: drawn by its points as the
    // mesh is (their turns from the points around them).
    Sheet sheet = MakeSheet();
    hyoshi::renderer::ClothSettings held = Loose();
    held.Gravity = 0.0f;
    held.Shape = {1.0f, 1.0f};
    held.Inertia = 0.0f;
    hyoshi::renderer::MeshCloth cloth;
    cloth.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, held, 1);
    const std::vector<hyoshi::renderer::MeshVertex> still = Run(sheet, cloth, 30, Still);
    for (size_t v = 0; v < still.size(); ++v)
    {
        CHECK(glm::distance(still[v].Position, sheet.Mesh.Vertices[v].Position) < 1.0e-3f);
        CHECK(still[v].Normal.y == doctest::Approx(1.0f).epsilon(0.01));
    }

    // The body turned a quarter about y over a second and moved along x: carried along, the cloth is
    // where the body's turn puts the mesh, facing as it does.
    hyoshi::renderer::MeshCloth carried;
    carried.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, held, 1);
    const auto turn = [](hyoshi::renderer::ModelNode& root, int frame)
    {
        const float through = std::min(static_cast<float>(frame) / 60.0f, 1.0f);
        root.Rotation = glm::angleAxis(through * glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        root.Translation = {through * 0.5f, 0.0f, 0.0f};
    };
    const std::vector<hyoshi::renderer::MeshVertex> moved = Run(sheet, carried, 90, turn);
    const glm::mat4 body = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f)) *
                           glm::mat4_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    for (size_t v = 0; v < moved.size(); ++v)
    {
        CHECK(glm::distance(moved[v].Position, glm::vec3(body * glm::vec4(sheet.Mesh.Vertices[v].Position, 1.0f))) <
              2.0e-3f);
    }
}

TEST_CASE("MeshCloth hangs under gravity, keeping its length, unless held to its shape or reach")
{
    Sheet sheet = MakeSheet();
    const size_t farCorner = sheet.Mesh.Vertices.size() - 1;
    const float length = static_cast<float>(ALONG - 1) * SPACING;
    hyoshi::renderer::MeshCloth loose;
    loose.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, Loose(), 1);
    const std::vector<hyoshi::renderer::MeshVertex> hanging = Run(sheet, loose, 240, Still);
    // Down from its fixed edge, about as long as it is (stretching a little, as cloth does).
    CHECK(hanging[farCorner].Position.y < -length * 0.9f);
    CHECK(hanging[farCorner].Position.y > -length * 1.1f);
    CHECK(std::abs(hanging[farCorner].Position.z) < length * 0.2f);
    // Its fixed edge where it was.
    CHECK(glm::distance(hanging[ACROSS - 1].Position, sheet.Mesh.Vertices[ACROSS - 1].Position) < 1.0e-4f);

    // Held to its shape as modeled, it hardly droops.
    hyoshi::renderer::ClothSettings shaped = Loose();
    shaped.Shape = {1.0f, 1.0f};
    hyoshi::renderer::MeshCloth held;
    held.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, shaped, 1);
    CHECK(Run(sheet, held, 240, Still)[farCorner].Position.y > -0.03f);

    // Reaching 5 cm from where it's modeled at most, it droops no farther.
    hyoshi::renderer::ClothSettings close = Loose();
    close.Reach = {0.05f, 0.05f};
    hyoshi::renderer::MeshCloth reaching;
    reaching.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, close, 1);
    CHECK(Run(sheet, reaching, 240, Still)[farCorner].Position.y > -0.051f);
}

TEST_CASE("MeshCloth lies over colliders and a floor, its edges too, and the same way every time")
{
    // A ball under the sheet's middle: it drapes over it, none of it inside.
    Sheet sheet = MakeSheet();
    const glm::vec3 ball{0.25f, -0.2f, 0.25f};
    constexpr float BALL_RADIUS = 0.15f;
    hyoshi::renderer::ClothCollider collider;
    collider.Shape = hyoshi::renderer::ColliderShape::Ball;
    collider.From = ball;
    collider.FromRadius = BALL_RADIUS;
    const auto drape = [&sheet, &collider](std::optional<float> floor)
    {
        hyoshi::renderer::MeshCloth cloth;
        cloth.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, Loose(), 1);
        cloth.SetColliders(sheet.Model, std::span(&collider, 1));
        cloth.SetFloor(floor);
        Run(sheet, cloth, 240, Still);
        return std::vector<glm::vec3>(cloth.GetPoints().begin(), cloth.GetPoints().end());
    };
    const std::vector<glm::vec3> draped = drape(std::nullopt);
    const float radius = Loose().Radius;
    bool isOnBall = false;
    for (const glm::vec3 point : draped)
    {
        CHECK(glm::distance(point, ball) > BALL_RADIUS + radius - 2.0e-3f);
        isOnBall = isOnBall || glm::distance(point, ball) < BALL_RADIUS + radius + 0.01f;
    }
    CHECK(isOnBall);
    // Again, the same.
    const std::vector<glm::vec3> again = drape(std::nullopt);
    for (size_t i = 0; i < draped.size(); ++i)
    {
        CHECK(glm::distance(draped[i], again[i]) < 1.0e-6f);
    }
    // A floor 10 cm down: none of it under the floor.
    for (const glm::vec3 point : drape(-0.1f))
    {
        CHECK(point.y > -0.1f + radius - 1.0e-3f);
    }

    // A thin bar across under the sheet, between two rows of points: kept out by its edges too, the
    // edges between them lie on it (with only its points kept out, it would pass between them).
    hyoshi::renderer::ClothCollider bar;
    bar.From = {-0.2f, -0.05f, 0.15f};
    bar.To = {0.7f, -0.05f, 0.15f};
    bar.FromRadius = 0.01f;
    bar.ToRadius = 0.01f;
    hyoshi::renderer::ClothSettings byEdges = Loose();
    byEdges.Edges = true;
    hyoshi::renderer::MeshCloth onBar;
    onBar.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, byEdges, 1);
    onBar.SetColliders(sheet.Model, std::span(&bar, 1));
    Run(sheet, onBar, 240, Still);
    // It hangs from the bar (16 cm along it), not from its fixed edge: its far corner about 30 cm
    // down, not 40, and the row past the bar (20 cm along) just under it, not 20 cm down.
    CHECK(onBar.GetPoints().back().y > -0.34f);
    CHECK(onBar.GetPoints()[ACROSS * 2].y > -0.14f);
}

TEST_CASE("MeshCloth doesn't fling an edge's end for a touch beside its fixed end, in a collider")
{
    // The sheet held still (no gravity), its fixed edge inside a thin capsule along it (as a coat's
    // waist is in a body's hips), the capsule a hair ahead of it: its first edges, from inside it out,
    // touch it a thousandth of the way along, deep in. Their moving ends stay where they are (pushed
    // for that touch, they'd go a thousand times as far as it's deep: metres).
    Sheet sheet = MakeSheet();
    hyoshi::renderer::ClothCollider bar;
    bar.From = {-0.2f, -0.0099f, 0.0001f};
    bar.To = {0.7f, -0.0099f, 0.0001f};
    bar.FromRadius = 0.02f;
    bar.ToRadius = 0.02f;
    hyoshi::renderer::ClothSettings still = Loose();
    still.Gravity = 0.0f;
    still.Reach = {5.0f, 5.0f};
    still.Edges = true;
    hyoshi::renderer::MeshCloth cloth;
    cloth.Build(sheet.Model, sheet.Mesh, sheet.Skin, sheet.Paint, still, 1);
    cloth.SetColliders(sheet.Model, std::span(&bar, 1));
    Run(sheet, cloth, 30, Still);
    for (size_t point = ACROSS; point < cloth.GetPoints().size(); ++point)
    {
        CHECK(glm::distance(cloth.GetPoints()[point], sheet.Mesh.Vertices[point].Position) < 0.01f);
    }
}
