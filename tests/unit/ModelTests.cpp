#include "renderer/Model.h"
#include "renderer/Skinning.h"
#include "renderer/SpringBones.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace
{

// One triangle (0,0,0), (1,0,0), (0,1,0), indexed, without normals, placed twice: moved to (1, 2,
// 3), and mirrored across x. One material: orange, two-sided.
constexpr std::string_view TRIANGLES =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0,"translation":[1,2,3]},)"
    R"({"mesh":0,"scale":[-1,1,1]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],)"
    R"("materials":[{"name":"Robe","pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0,1]},"doubleSided":true,)"
    R"("extras":{"toon":1}}],)"
    R"("buffers":[{"byteLength":44,"uri":"data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";

// The triangle (unplaced) with two blend shapes: "smile" lifts its second corner by half, and "wink",
// sparse (as Blender writes them), moves its third a quarter across.
constexpr std::string_view SMILING =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":2},{"POSITION":3}]}],)"
    R"("extras":{"targetNames":["smile","wink"]}}],)"
    R"("buffers":[{"byteLength":96,"uri":"data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPwAAAAAAAAAAAAAAAAAAAAACAAAAAACAPgAAAAAAAAAA"}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},)"
    R"({"buffer":0,"byteOffset":44,"byteLength":36},{"buffer":0,"byteOffset":80,"byteLength":2},)"
    R"({"buffer":0,"byteOffset":84,"byteLength":12}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},)"
    R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0.5,0]},)"
    R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0.25,0,0],)"
    R"("sparse":{"count":1,"indices":{"bufferView":3,"componentType":5123},"values":{"bufferView":4}}}]})";

// The triangle skinned to one joint, "Bone", at (0, 1, 0) under "Armature" at (0, 0, 5) (its inverse
// bind undoing both), the mesh's node moved (which glTF ignores for a skinned mesh); "Turn" turns the
// bone a quarter about z over a second.
constexpr std::string_view SKINNED =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2]}],)"
    R"("nodes":[{"name":"Armature","translation":[0,0,5],"children":[1]},{"name":"Bone","translation":[0,1,0]},)"
    R"({"mesh":0,"skin":0,"translation":[7,7,7]}],"skins":[{"joints":[1],"inverseBindMatrices":4}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]}],)"
    R"("animations":[{"name":"Turn","channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}],)"
    R"("samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}]}],)"
    R"("buffers":[{"byteLength":208,"uri":"data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAA)"
    R"(AAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAA)"
    R"(AACAvwAAoMAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAADzBDU/8wQ1Pw=="}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},)"
    R"({"buffer":0,"byteOffset":44,"byteLength":12},{"buffer":0,"byteOffset":56,"byteLength":48},)"
    R"({"buffer":0,"byteOffset":104,"byteLength":64},{"buffer":0,"byteOffset":168,"byteLength":8},)"
    R"({"buffer":0,"byteOffset":176,"byteLength":32}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},)"
    R"({"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},)"
    R"({"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},)"
    R"({"bufferView":4,"componentType":5126,"count":1,"type":"MAT4"},)"
    R"({"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
    R"({"bufferView":6,"componentType":5126,"count":2,"type":"VEC4"}]})";

// A .glb (as Blender exports): one triangle with normals and texture coordinates, its material a
// 2x2 PNG inside the file (red, green, blue, and half-transparent white), blended.
constexpr std::array<uint8_t, 1024> TEXTURED_GLB{
    0x67, 0x6C, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x34, 0x03, 0x00, 0x00, 0x4A, 0x53, 0x4F,
    0x4E, 0x7B, 0x22, 0x61, 0x73, 0x73, 0x65, 0x74, 0x22, 0x3A, 0x7B, 0x22, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E,
    0x22, 0x3A, 0x22, 0x32, 0x2E, 0x30, 0x22, 0x7D, 0x2C, 0x22, 0x73, 0x63, 0x65, 0x6E, 0x65, 0x22, 0x3A, 0x30, 0x2C,
    0x22, 0x73, 0x63, 0x65, 0x6E, 0x65, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x6E, 0x6F, 0x64, 0x65, 0x73, 0x22, 0x3A,
    0x5B, 0x30, 0x5D, 0x7D, 0x5D, 0x2C, 0x22, 0x6E, 0x6F, 0x64, 0x65, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x6D, 0x65,
    0x73, 0x68, 0x22, 0x3A, 0x30, 0x7D, 0x5D, 0x2C, 0x22, 0x6D, 0x65, 0x73, 0x68, 0x65, 0x73, 0x22, 0x3A, 0x5B, 0x7B,
    0x22, 0x70, 0x72, 0x69, 0x6D, 0x69, 0x74, 0x69, 0x76, 0x65, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x61, 0x74, 0x74,
    0x72, 0x69, 0x62, 0x75, 0x74, 0x65, 0x73, 0x22, 0x3A, 0x7B, 0x22, 0x50, 0x4F, 0x53, 0x49, 0x54, 0x49, 0x4F, 0x4E,
    0x22, 0x3A, 0x30, 0x2C, 0x22, 0x4E, 0x4F, 0x52, 0x4D, 0x41, 0x4C, 0x22, 0x3A, 0x31, 0x2C, 0x22, 0x54, 0x45, 0x58,
    0x43, 0x4F, 0x4F, 0x52, 0x44, 0x5F, 0x30, 0x22, 0x3A, 0x32, 0x7D, 0x2C, 0x22, 0x6D, 0x61, 0x74, 0x65, 0x72, 0x69,
    0x61, 0x6C, 0x22, 0x3A, 0x30, 0x7D, 0x5D, 0x7D, 0x5D, 0x2C, 0x22, 0x6D, 0x61, 0x74, 0x65, 0x72, 0x69, 0x61, 0x6C,
    0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x6E, 0x61, 0x6D, 0x65, 0x22, 0x3A, 0x22, 0x50, 0x61, 0x67, 0x65, 0x22, 0x2C,
    0x22, 0x70, 0x62, 0x72, 0x4D, 0x65, 0x74, 0x61, 0x6C, 0x6C, 0x69, 0x63, 0x52, 0x6F, 0x75, 0x67, 0x68, 0x6E, 0x65,
    0x73, 0x73, 0x22, 0x3A, 0x7B, 0x22, 0x62, 0x61, 0x73, 0x65, 0x43, 0x6F, 0x6C, 0x6F, 0x72, 0x54, 0x65, 0x78, 0x74,
    0x75, 0x72, 0x65, 0x22, 0x3A, 0x7B, 0x22, 0x69, 0x6E, 0x64, 0x65, 0x78, 0x22, 0x3A, 0x30, 0x7D, 0x7D, 0x2C, 0x22,
    0x61, 0x6C, 0x70, 0x68, 0x61, 0x4D, 0x6F, 0x64, 0x65, 0x22, 0x3A, 0x22, 0x42, 0x4C, 0x45, 0x4E, 0x44, 0x22, 0x7D,
    0x5D, 0x2C, 0x22, 0x74, 0x65, 0x78, 0x74, 0x75, 0x72, 0x65, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x73, 0x6F, 0x75,
    0x72, 0x63, 0x65, 0x22, 0x3A, 0x30, 0x7D, 0x5D, 0x2C, 0x22, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, 0x22, 0x3A, 0x5B,
    0x7B, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x56, 0x69, 0x65, 0x77, 0x22, 0x3A, 0x33, 0x2C, 0x22, 0x6D, 0x69,
    0x6D, 0x65, 0x54, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x22, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x2F, 0x70, 0x6E, 0x67, 0x22,
    0x7D, 0x5D, 0x2C, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x62, 0x79, 0x74,
    0x65, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x22, 0x3A, 0x31, 0x37, 0x36, 0x7D, 0x5D, 0x2C, 0x22, 0x62, 0x75, 0x66,
    0x66, 0x65, 0x72, 0x56, 0x69, 0x65, 0x77, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72,
    0x22, 0x3A, 0x30, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65, 0x4F, 0x66, 0x66, 0x73, 0x65, 0x74, 0x22, 0x3A, 0x30, 0x2C,
    0x22, 0x62, 0x79, 0x74, 0x65, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x22, 0x3A, 0x33, 0x36, 0x7D, 0x2C, 0x7B, 0x22,
    0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x22, 0x3A, 0x30, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65, 0x4F, 0x66, 0x66, 0x73,
    0x65, 0x74, 0x22, 0x3A, 0x33, 0x36, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x22,
    0x3A, 0x33, 0x36, 0x7D, 0x2C, 0x7B, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x22, 0x3A, 0x30, 0x2C, 0x22, 0x62,
    0x79, 0x74, 0x65, 0x4F, 0x66, 0x66, 0x73, 0x65, 0x74, 0x22, 0x3A, 0x37, 0x32, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65,
    0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x22, 0x3A, 0x32, 0x34, 0x7D, 0x2C, 0x7B, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65,
    0x72, 0x22, 0x3A, 0x30, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65, 0x4F, 0x66, 0x66, 0x73, 0x65, 0x74, 0x22, 0x3A, 0x39,
    0x36, 0x2C, 0x22, 0x62, 0x79, 0x74, 0x65, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x22, 0x3A, 0x37, 0x37, 0x7D, 0x5D,
    0x2C, 0x22, 0x61, 0x63, 0x63, 0x65, 0x73, 0x73, 0x6F, 0x72, 0x73, 0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x62, 0x75, 0x66,
    0x66, 0x65, 0x72, 0x56, 0x69, 0x65, 0x77, 0x22, 0x3A, 0x30, 0x2C, 0x22, 0x63, 0x6F, 0x6D, 0x70, 0x6F, 0x6E, 0x65,
    0x6E, 0x74, 0x54, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x35, 0x31, 0x32, 0x36, 0x2C, 0x22, 0x63, 0x6F, 0x75, 0x6E, 0x74,
    0x22, 0x3A, 0x33, 0x2C, 0x22, 0x74, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x22, 0x56, 0x45, 0x43, 0x33, 0x22, 0x2C, 0x22,
    0x6D, 0x69, 0x6E, 0x22, 0x3A, 0x5B, 0x30, 0x2C, 0x30, 0x2C, 0x30, 0x5D, 0x2C, 0x22, 0x6D, 0x61, 0x78, 0x22, 0x3A,
    0x5B, 0x31, 0x2C, 0x31, 0x2C, 0x30, 0x5D, 0x7D, 0x2C, 0x7B, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x56, 0x69,
    0x65, 0x77, 0x22, 0x3A, 0x31, 0x2C, 0x22, 0x63, 0x6F, 0x6D, 0x70, 0x6F, 0x6E, 0x65, 0x6E, 0x74, 0x54, 0x79, 0x70,
    0x65, 0x22, 0x3A, 0x35, 0x31, 0x32, 0x36, 0x2C, 0x22, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x22, 0x3A, 0x33, 0x2C, 0x22,
    0x74, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x22, 0x56, 0x45, 0x43, 0x33, 0x22, 0x7D, 0x2C, 0x7B, 0x22, 0x62, 0x75, 0x66,
    0x66, 0x65, 0x72, 0x56, 0x69, 0x65, 0x77, 0x22, 0x3A, 0x32, 0x2C, 0x22, 0x63, 0x6F, 0x6D, 0x70, 0x6F, 0x6E, 0x65,
    0x6E, 0x74, 0x54, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x35, 0x31, 0x32, 0x36, 0x2C, 0x22, 0x63, 0x6F, 0x75, 0x6E, 0x74,
    0x22, 0x3A, 0x33, 0x2C, 0x22, 0x74, 0x79, 0x70, 0x65, 0x22, 0x3A, 0x22, 0x56, 0x45, 0x43, 0x32, 0x22, 0x7D, 0x5D,
    0x7D, 0x20, 0x20, 0x20, 0xB0, 0x00, 0x00, 0x00, 0x42, 0x49, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80,
    0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A,
    0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08,
    0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
    0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x04, 0x59, 0x18, 0xC1, 0x34, 0x43, 0x23, 0x00, 0x3F, 0x80, 0x06, 0x81, 0xC4, 0x55,
    0x98, 0x41, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82, 0x00, 0x00, 0x00,
};

std::span<const std::byte> AsBytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

} // namespace

TEST_CASE("ReadModel places a glTF's meshes by their nodes, with its materials")
{
    hyoshi::Result<hyoshi::renderer::ModelData> model = hyoshi::renderer::ReadModel(AsBytes(TRIANGLES));
    REQUIRE(model);
    REQUIRE(model.Value().Parts.size() == 1);
    const hyoshi::renderer::ModelPart& part = model.Value().Parts[0];
    CHECK(part.Name == "Robe");
    CHECK(part.Extras == R"({"toon":1})");
    CHECK(part.IsTwoSided);
    CHECK_FALSE(part.IsSeeThrough);
    // Linear 0.5 is sRGB 186.
    CHECK(part.BaseColor.R == 255);
    CHECK(part.BaseColor.G == 186);
    CHECK(part.BaseColor.B == 0);
    CHECK(part.Texture.IsEmpty());
    REQUIRE(part.Mesh.Vertices.size() == 6);
    REQUIRE(part.Mesh.Indices.size() == 6);

    // Moved: the first corner at (1, 2, 3), its normal made from the triangle, facing +z.
    CHECK(part.Mesh.Vertices[0].Position.x == doctest::Approx(1.0f));
    CHECK(part.Mesh.Vertices[0].Position.y == doctest::Approx(2.0f));
    CHECK(part.Mesh.Vertices[0].Position.z == doctest::Approx(3.0f));
    CHECK(part.Mesh.Vertices[1].Position.x == doctest::Approx(2.0f));
    CHECK(part.Mesh.Vertices[0].Normal.z == doctest::Approx(1.0f));

    // Mirrored: still facing +z, its winding turned back.
    CHECK(part.Mesh.Vertices[4].Position.x == doctest::Approx(-1.0f));
    CHECK(part.Mesh.Vertices[3].Normal.z == doctest::Approx(1.0f));
    CHECK(part.Mesh.Indices[3] == 3);
    CHECK(part.Mesh.Indices[4] == 5);
    CHECK(part.Mesh.Indices[5] == 4);
}

TEST_CASE("ReadModel refuses what isn't a glTF model")
{
    CHECK_FALSE(hyoshi::renderer::ReadModel(AsBytes("not a model")));
    CHECK_FALSE(hyoshi::renderer::ReadModel(AsBytes(R"({"asset":{"version":"2.0"}})")));
}

TEST_CASE("ReadModel reads a .glb's texture")
{
    hyoshi::Result<hyoshi::renderer::ModelData> model =
        hyoshi::renderer::ReadModel(std::as_bytes(std::span(TEXTURED_GLB)));
    REQUIRE(model);
    REQUIRE(model.Value().Parts.size() == 1);
    const hyoshi::renderer::ModelPart& part = model.Value().Parts[0];
    CHECK(part.Name == "Page");
    CHECK(part.IsSeeThrough);
    REQUIRE(part.Texture.Width == 2);
    REQUIRE(part.Texture.Height == 2);
    // The first pixel red, the last half-transparent.
    CHECK(part.Texture.Pixels[0] == 255);
    CHECK(part.Texture.Pixels[1] == 0);
    CHECK(part.Texture.Pixels[15] == 128);
    REQUIRE(part.Mesh.Vertices.size() == 3);
    CHECK(part.Mesh.Vertices[0].Uv.y == doctest::Approx(1.0f));
    CHECK(part.Mesh.Vertices[1].Normal.z == doctest::Approx(1.0f));
}

TEST_CASE("ReadModel reads blend shapes by name, dense or sparse, only the vertices they move")
{
    const hyoshi::Result<hyoshi::renderer::ModelData> model = hyoshi::renderer::ReadModel(AsBytes(SMILING));
    REQUIRE(model);
    REQUIRE(model.Value().Parts.size() == 1);
    const std::vector<hyoshi::renderer::MorphTarget>& morphs = model.Value().Parts[0].Morphs;
    REQUIRE(morphs.size() == 2);
    CHECK(morphs[0].Name == "smile");
    REQUIRE(morphs[0].Moves.size() == 1);
    CHECK(morphs[0].Moves[0].first == 1);
    CHECK(morphs[0].Moves[0].second.y == doctest::Approx(0.5f));
    CHECK(morphs[1].Name == "wink");
    REQUIRE(morphs[1].Moves.size() == 1);
    CHECK(morphs[1].Moves[0].first == 2);
    CHECK(morphs[1].Moves[0].second.x == doctest::Approx(0.25f));
}

TEST_CASE("ReadModel reads a skin and its animation, which PoseNodes and SkinVertices play")
{
    hyoshi::Result<hyoshi::renderer::ModelData> read = hyoshi::renderer::ReadModel(AsBytes(SKINNED));
    REQUIRE(read);
    hyoshi::renderer::ModelData& model = read.Value();
    REQUIRE(model.Nodes.size() == 3);
    CHECK(model.Nodes[0].Name == "Armature");
    CHECK(model.Nodes[1].Parent == 0);
    REQUIRE(model.Joints.size() == 1);
    CHECK(model.Joints[0].Node == 1);
    REQUIRE(model.Parts.size() == 1);
    const hyoshi::renderer::ModelPart& part = model.Parts[0];
    REQUIRE(part.Skin.size() == 3);
    CHECK(part.Skin[1].Weights.x == doctest::Approx(1.0f));
    // Where it was bound, not where its node is.
    CHECK(part.Mesh.Vertices[1].Position.x == doctest::Approx(1.0f));
    const hyoshi::renderer::ModelAnimation* turn = hyoshi::renderer::FindAnimation(model, "Turn");
    REQUIRE(turn != nullptr);
    CHECK(turn->Duration == doctest::Approx(1.0f));
    CHECK(hyoshi::renderer::FindAnimation(model, "Walk") == nullptr);

    std::vector<glm::mat4> nodes;
    std::vector<glm::mat4> joints;
    std::vector<hyoshi::renderer::MeshVertex> posed(3);
    const auto corner = [&](float time)
    {
        hyoshi::renderer::PoseNodes(model, turn, time, nodes);
        hyoshi::renderer::PoseJoints(model, nodes, joints);
        hyoshi::renderer::SkinVertices(part.Mesh.Vertices, part.Skin, joints, posed);
        return posed[1].Position;
    };
    // At rest, as bound; half way, an eighth turn about the bone; at the end a quarter; past it, round again.
    CHECK(corner(0.0f).x == doctest::Approx(1.0f));
    CHECK(corner(0.0f).y == doctest::Approx(0.0f));
    CHECK(corner(0.5f).x == doctest::Approx(1.41421f));
    CHECK(corner(0.5f).y == doctest::Approx(1.0f));
    CHECK(corner(1.5f).x == doctest::Approx(1.41421f));
    CHECK(corner(0.999f).x == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(corner(0.999f).y == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(posed[1].Normal.z == doctest::Approx(1.0f));

    hyoshi::renderer::ApplyPose(model, turn, 0.5f);
    CHECK(model.Parts[0].Skin.empty());
    CHECK(model.Parts[0].Mesh.Vertices[1].Position.x == doctest::Approx(1.41421f));
}

TEST_CASE("SpringBones swing a chain down under its pull, keeping its length, and hold it without one")
{
    // A bone along x from the origin, its tip a meter out, and a root above both.
    hyoshi::renderer::ModelData model;
    model.Nodes = {{"Root", -1}, {"Tail", 0}, {"Tip", 1}};
    model.Nodes[2].Translation = {1.0f, 0.0f, 0.0f};
    const auto settle = [&model](hyoshi::renderer::SpringSettings settings)
    {
        hyoshi::renderer::SpringBones springs;
        CHECK(springs.Add(model, "Tail", settings) == 1);
        CHECK(springs.GetCount() == 1);
        std::vector<glm::mat4> nodes;
        for (int frame = 0; frame < 240; ++frame)
        {
            hyoshi::renderer::PoseNodes(model, nullptr, 0.0f, nodes);
            springs.Step(model, nodes, 1.0f / 60.0f);
        }
        return glm::vec3(nodes[2][3]);
    };
    const glm::vec3 hanging = settle({0.0f, 0.4f, 10.0f});
    CHECK(glm::length(hanging) == doctest::Approx(1.0f));
    CHECK(hanging.y < -0.99f);
    // Pulled back as hard as down, it settles half way.
    const glm::vec3 between = settle({10.0f, 0.4f, 10.0f});
    CHECK(between.x == doctest::Approx(0.7071f).epsilon(0.02));
    CHECK(between.y == doctest::Approx(-0.7071f).epsilon(0.02));
    const glm::vec3 held = settle({1.0f, 0.4f, 0.0f});
    CHECK(held.x == doctest::Approx(1.0f));
    CHECK(held.y == doctest::Approx(0.0f));
    hyoshi::renderer::SpringBones none;
    CHECK(none.Add(model, "Skirt", {}) == 0);
}

TEST_CASE("SpringBones keep a chain out of a collider and above the floor")
{
    // A bone along x from the origin, a meter long, pulled down; a ball under its tip, and a floor.
    hyoshi::renderer::ModelData model;
    model.Nodes = {{"Root", -1}, {"Tail", 0}, {"Tip", 1}, {"Ball", 0}};
    model.Nodes[2].Translation = {1.0f, 0.0f, 0.0f};
    model.Nodes[3].Translation = {1.0f, -1.0f, 0.0f};
    const auto settle = [&model](bool isBall, std::optional<float> floor)
    {
        hyoshi::renderer::SpringBones springs;
        springs.Add(model, "Tail", {0.0f, 0.4f, 10.0f, 0.1f});
        if (isBall)
        {
            CHECK(springs.AddCollider(model, "Ball", "", 0.6f));
        }
        springs.SetFloor(floor);
        std::vector<glm::mat4> nodes;
        for (int frame = 0; frame < 240; ++frame)
        {
            hyoshi::renderer::PoseNodes(model, nullptr, 0.0f, nodes);
            springs.Step(model, nodes, 1.0f / 60.0f);
        }
        return glm::vec3(nodes[2][3]);
    };
    // Hanging straight down, its tip would be in the ball: it rests on it, as far out as the two are
    // thick (or nearly, kept at its length).
    const glm::vec3 onBall = settle(true, std::nullopt);
    CHECK(glm::length(onBall) == doctest::Approx(1.0f));
    CHECK(glm::distance(onBall, glm::vec3(1.0f, -1.0f, 0.0f)) > 0.6f);
    // A floor half a meter down: its tip as far above it as it's thick.
    const glm::vec3 onFloor = settle(false, -0.5f);
    CHECK(onFloor.y == doctest::Approx(-0.4f).epsilon(0.02));
    hyoshi::renderer::SpringBones springs;
    CHECK_FALSE(springs.AddCollider(model, "Leg", "", 0.1f));
}

TEST_CASE("SpringBones lay a chain on a seat and over its edge, and keep a bone beside it out of its side")
{
    // A seat's top 0.2 m under the root, its edge half a meter out along x.
    const hyoshi::renderer::SpringSeat seat{{-1.0f, -1.0f}, {0.5f, 1.0f}, -0.2f};
    const auto settle =
        [&seat](const hyoshi::renderer::ModelData& model, hyoshi::renderer::SpringSettings settings, bool hasSeat)
    {
        hyoshi::renderer::SpringBones springs;
        springs.Add(model, "Tail", settings);
        if (hasSeat)
        {
            springs.SetSeat(seat);
        }
        std::vector<glm::mat4> nodes;
        for (int frame = 0; frame < 240; ++frame)
        {
            hyoshi::renderer::PoseNodes(model, nullptr, 0.0f, nodes);
            springs.Step(model, nodes, 1.0f / 60.0f);
        }
        return nodes;
    };
    // Three bones of 0.3 m along x from the root, pulled down: they come down onto the top, along it,
    // and over its edge, each at its length, none under the top over the seat.
    hyoshi::renderer::ModelData chain;
    chain.Nodes = {{"Root", -1}, {"Tail", 0}, {"Middle", 1}, {"End", 2}, {"Tip", 3}};
    for (size_t node = 2; node < chain.Nodes.size(); ++node)
    {
        chain.Nodes[node].Translation = {0.3f, 0.0f, 0.0f};
    }
    const std::vector<glm::mat4> laid = settle(chain, {0.0f, 0.4f, 10.0f, 0.05f}, true);
    for (size_t node = 2; node < chain.Nodes.size(); ++node)
    {
        const glm::vec3 at(laid[node][3]);
        CHECK(glm::distance(at, glm::vec3(laid[node - 1][3])) == doctest::Approx(0.3f));
        CHECK((at.x > 0.5f || at.y >= -0.2f));
    }
    const glm::vec3 end(laid[4][3]);
    CHECK(end.x > 0.5f);
    CHECK(end.y < -0.35f);
    // Without the seat, it hangs straight down.
    CHECK(glm::vec3(settle(chain, {0.0f, 0.4f, 10.0f, 0.05f}, false)[4][3]).y < -0.89f);
    // A bone beside the seat, under its top, pointing down into it: kept out of its side.
    hyoshi::renderer::ModelData beside;
    beside.Nodes = {{"Root", -1}, {"Tail", 0}, {"Tip", 1}};
    beside.Nodes[1].Translation = {0.6f, -0.3f, 0.0f};
    beside.Nodes[2].Translation = {-0.2121f, -0.2121f, 0.0f};
    CHECK(glm::vec3(settle(beside, {10.0f, 0.4f, 0.0f, 0.05f}, false)[2][3]).x < 0.45f);
    CHECK(glm::vec3(settle(beside, {10.0f, 0.4f, 0.0f, 0.05f}, true)[2][3]).x > 0.5f);
}

TEST_CASE("SpringBones rest a bone from a seat's top on its edge, still, and start at rest")
{
    // A bone of 0.1 m along x, its head on a seat's top (or a little under it, as a bone lying on it
    // ends), the edge 4 cm out, pulled down.
    hyoshi::renderer::ModelData model;
    model.Nodes = {{"Root", -1}, {"Tail", 0}, {"Tip", 1}};
    model.Nodes[2].Translation = {0.1f, 0.0f, 0.0f};
    for (const float top : {-0.005f, 0.002f})
    {
        hyoshi::renderer::SpringBones springs;
        springs.Add(model, "Tail", {0.0f, 0.4f, 10.0f});
        springs.SetSeat(hyoshi::renderer::SpringSeat{{-1.0f, -1.0f}, {0.04f, 1.0f}, top});
        std::vector<glm::mat4> nodes;
        std::vector<glm::vec3> tips;
        for (int frame = 0; frame < 120; ++frame)
        {
            hyoshi::renderer::PoseNodes(model, nullptr, 0.0f, nodes);
            springs.Step(model, nodes, 1.0f / 60.0f);
            tips.emplace_back(nodes[2][3]);
        }
        // Over the edge, resting on it (not hanging down beside it, nor through it), from the first
        // frame on (it came to rest as it started), not moving.
        CHECK(tips.back().x > 0.04f);
        CHECK(tips.back().y > -0.02f);
        CHECK(glm::length(tips.back()) == doctest::Approx(0.1f));
        for (const glm::vec3& tip : tips)
        {
            CHECK(glm::distance(tip, tips.back()) < 1.0e-4f);
        }
    }
}

TEST_CASE("SpringBones move the bones every frame, between their steps, at any frame rate")
{
    // A bone hanging under a root that moves along x at half a meter a second, at 60 and at 144 frames
    // a second.
    hyoshi::renderer::ModelData model;
    model.Nodes = {{"Root", -1}, {"Tail", 0}, {"Tip", 1}};
    model.Nodes[2].Translation = {0.0f, -0.3f, 0.0f};
    const auto follow = [&model](float framesASecond)
    {
        hyoshi::renderer::SpringBones springs;
        springs.Add(model, "Tail", {1.0f, 0.2f, 2.0f});
        std::vector<glm::mat4> nodes;
        std::vector<glm::vec3> tips;
        const auto frames = static_cast<int>(framesASecond);
        for (int frame = 0; frame <= frames; ++frame)
        {
            model.Nodes[0].Translation.x = 0.5f * static_cast<float>(frame) / framesASecond;
            hyoshi::renderer::PoseNodes(model, nullptr, 0.0f, nodes);
            springs.Step(model, nodes, 1.0f / framesASecond);
            tips.emplace_back(nodes[2][3]);
        }
        return tips;
    };
    const std::vector<glm::vec3> quick = follow(144.0f);
    // Dragged along steadily, it goes on as the root does every frame, though most don't step (a step
    // is a sixtieth of a second): not still on some and catching up on others.
    const float each = 0.5f / 144.0f;
    for (size_t frame = quick.size() / 2; frame < quick.size(); ++frame)
    {
        CHECK(std::abs(quick[frame].x - quick[frame - 1].x - each) < each * 0.2f);
    }
    // Where it is after the second, as at 60 frames a second, near enough.
    CHECK(glm::distance(quick.back(), follow(60.0f).back()) < 0.01f);
    model.Nodes[0].Translation = glm::vec3(0.0f);
}
