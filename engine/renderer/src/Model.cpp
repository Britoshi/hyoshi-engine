#include "renderer/Model.h"

#include "core/Log.h"

#include <cgltf.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace hyoshi::renderer
{
namespace
{

struct DataDeleter
{
    void operator()(cgltf_data* data) const
    {
        cgltf_free(data);
    }
};
using DataPointer = std::unique_ptr<cgltf_data, DataDeleter>;

// glTF's colors are linear; Material's and MeshVertex's are sRGB.
uint8_t ToSrgbByte(float linear)
{
    const float srgb = std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f);
    return static_cast<uint8_t>(std::lround(srgb * 255.0f));
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

// An image's file bytes: inside the .glb, in a data: URI, or in a file beside the model.
std::vector<std::byte> ReadImageBytes(const cgltf_image& image, const std::string& modelPath)
{
    if (image.buffer_view != nullptr && image.buffer_view->buffer->data != nullptr)
    {
        const auto* start = static_cast<const std::byte*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        return {start, start + image.buffer_view->size};
    }
    if (image.uri == nullptr)
    {
        return {};
    }
    const std::string_view uri(image.uri);
    if (uri.starts_with("data:"))
    {
        const size_t comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
        {
            return {};
        }
        const std::string_view base64 = uri.substr(comma + 1);
        const size_t padding = base64.ends_with("==") ? 2 : (base64.ends_with('=') ? 1 : 0);
        const size_t size = (base64.size() / 4 * 3) - padding;
        cgltf_options options{};
        void* decoded = nullptr;
        if (cgltf_load_buffer_base64(&options, size, base64.data(), &decoded) != cgltf_result_success)
        {
            return {};
        }
        const auto* start = static_cast<const std::byte*>(decoded);
        std::vector<std::byte> bytes(start, start + size);
        std::free(decoded); // NOLINT(cppcoreguidelines-no-malloc): cgltf allocated it with malloc
        return bytes;
    }
    if (modelPath.empty())
    {
        return {};
    }
    std::string file(uri);
    file.resize(cgltf_decode_uri(file.data()));
    return ReadFileBytes(std::filesystem::path(modelPath).parent_path() / file);
}

ModelPart MakePart(const cgltf_material* material, const std::string& modelPath)
{
    ModelPart part;
    if (material == nullptr)
    {
        part.Name = "default";
        return part;
    }
    part.Name = material->name != nullptr ? material->name : "material";
    part.Extras = material->extras.data != nullptr ? material->extras.data : "";
    part.IsTwoSided = material->double_sided != 0;
    part.IsSeeThrough = material->alpha_mode != cgltf_alpha_mode_opaque;
    if (material->has_pbr_metallic_roughness != 0)
    {
        const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
        part.BaseColor = {ToSrgbByte(pbr.base_color_factor[0]), ToSrgbByte(pbr.base_color_factor[1]),
                          ToSrgbByte(pbr.base_color_factor[2]),
                          static_cast<uint8_t>(std::lround(std::clamp(pbr.base_color_factor[3], 0.0f, 1.0f) * 255.0f))};
        const cgltf_texture* texture = pbr.base_color_texture.texture;
        if (texture != nullptr && texture->image != nullptr)
        {
            const std::vector<std::byte> bytes = ReadImageBytes(*texture->image, modelPath);
            Result<Image> image = bytes.empty() ? Result<Image>(Error{"not found"}) : DecodeImage(bytes);
            if (image)
            {
                part.Texture = std::move(image).Value();
            }
            else
            {
                HYOSHI_LOG_WARN("Model: material '{}': its texture didn't load ({})", part.Name,
                                image.GetError().Message);
            }
        }
    }
    return part;
}

class ModelReader
{
public:
    explicit ModelReader(std::string modelPath) : path(std::move(modelPath))
    {
    }

    void AddNode(const cgltf_node& node)
    {
        if (node.mesh != nullptr)
        {
            std::array<float, 16> world{};
            cgltf_node_transform_world(&node, world.data());
            if (node.skin != nullptr && !hasWarnedOfSkin)
            {
                HYOSHI_LOG_WARN("Model: '{}' is skinned, so it comes in its rest pose: pose it in Blender and "
                                "apply the armature before exporting",
                                node.name != nullptr ? node.name : "a mesh");
                hasWarnedOfSkin = true;
            }
            for (size_t i = 0; i < node.mesh->primitives_count; ++i)
            {
                AddPrimitive(*node.mesh, node.mesh->primitives[i], glm::make_mat4(world.data()));
            }
        }
        for (size_t i = 0; i < node.children_count; ++i)
        {
            AddNode(*node.children[i]);
        }
    }

    ModelData Finish()
    {
        if (skipped > 0)
        {
            HYOSHI_LOG_WARN("Model: skipped {} primitives that aren't triangles with positions", skipped);
        }
        return std::move(model);
    }

private:
    // The primitive's blend shapes, into its part's by name (a part gathers a material's primitives,
    // so a vertex's index is where the primitive's first landed, plus its own).
    static void AddMorphs(const cgltf_mesh& gltfMesh, const cgltf_primitive& primitive, const glm::mat4& transform,
                          ModelPart& part, uint32_t first)
    {
        const glm::mat3 turn(transform);
        for (size_t target = 0; target < primitive.targets_count; ++target)
        {
            const cgltf_morph_target& morph = primitive.targets[target];
            const cgltf_accessor* moves = nullptr;
            for (size_t i = 0; i < morph.attributes_count; ++i)
            {
                if (morph.attributes[i].type == cgltf_attribute_type_position)
                {
                    moves = morph.attributes[i].data;
                }
            }
            if (moves == nullptr)
            {
                continue;
            }
            const std::string name = target < gltfMesh.target_names_count && gltfMesh.target_names[target] != nullptr
                                         ? std::string(gltfMesh.target_names[target])
                                         : "target " + std::to_string(target);
            auto found = std::find_if(part.Morphs.begin(), part.Morphs.end(),
                                      [&name](const MorphTarget& known) { return known.Name == name; });
            if (found == part.Morphs.end())
            {
                part.Morphs.push_back({name, {}});
                found = std::prev(part.Morphs.end());
            }
            // Unpacked, as exporters often write a blend shape sparse (only the vertices it moves).
            std::vector<float> floats(moves->count * 3);
            cgltf_accessor_unpack_floats(moves, floats.data(), floats.size());
            for (size_t i = 0; i < moves->count; ++i)
            {
                const glm::vec3 move{floats[i * 3], floats[(i * 3) + 1], floats[(i * 3) + 2]};
                if (glm::dot(move, move) > 1.0e-14f)
                {
                    found->Moves.emplace_back(first + static_cast<uint32_t>(i), turn * move);
                }
            }
        }
    }

    void AddPrimitive(const cgltf_mesh& gltfMesh, const cgltf_primitive& primitive, const glm::mat4& transform)
    {
        const cgltf_accessor* positions = nullptr;
        const cgltf_accessor* normals = nullptr;
        const cgltf_accessor* uvs = nullptr;
        const cgltf_accessor* colors = nullptr;
        for (size_t i = 0; i < primitive.attributes_count; ++i)
        {
            const cgltf_attribute& attribute = primitive.attributes[i];
            if (attribute.type == cgltf_attribute_type_position)
            {
                positions = attribute.data;
            }
            else if (attribute.type == cgltf_attribute_type_normal)
            {
                normals = attribute.data;
            }
            else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
            {
                uvs = attribute.data;
            }
            else if (attribute.type == cgltf_attribute_type_color && attribute.index == 0)
            {
                colors = attribute.data;
            }
        }
        if (primitive.type != cgltf_primitive_type_triangles || positions == nullptr)
        {
            ++skipped;
            return;
        }

        const auto found = parts.find(primitive.material);
        const size_t partIndex = found != parts.end() ? found->second : model.Parts.size();
        if (found == parts.end())
        {
            parts.emplace(primitive.material, partIndex);
            model.Parts.push_back(MakePart(primitive.material, path));
        }
        MeshData& mesh = model.Parts[partIndex].Mesh;
        const auto first = static_cast<uint32_t>(mesh.Vertices.size());

        const glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(transform)));
        const size_t count = positions->count;
        for (size_t i = 0; i < count; ++i)
        {
            MeshVertex vertex;
            glm::vec3 position{0.0f};
            cgltf_accessor_read_float(positions, i, glm::value_ptr(position), 3);
            vertex.Position = glm::vec3(transform * glm::vec4(position, 1.0f));
            if (normals != nullptr && i < normals->count)
            {
                glm::vec3 normal{0.0f};
                cgltf_accessor_read_float(normals, i, glm::value_ptr(normal), 3);
                const glm::vec3 turned = normalTransform * normal;
                vertex.Normal = glm::length(turned) > 0.0f ? glm::normalize(turned) : glm::vec3(0.0f, 1.0f, 0.0f);
            }
            if (uvs != nullptr && i < uvs->count)
            {
                cgltf_accessor_read_float(uvs, i, glm::value_ptr(vertex.Uv), 2);
            }
            if (colors != nullptr && i < colors->count)
            {
                glm::vec4 color{1.0f};
                cgltf_accessor_read_float(colors, i, glm::value_ptr(color), cgltf_num_components(colors->type));
                vertex.Color =
                    PackMeshColor({ToSrgbByte(color.r), ToSrgbByte(color.g), ToSrgbByte(color.b),
                                   static_cast<uint8_t>(std::lround(std::clamp(color.a, 0.0f, 1.0f) * 255.0f))});
            }
            mesh.Vertices.push_back(vertex);
        }

        AddMorphs(gltfMesh, primitive, transform, model.Parts[partIndex], first);

        // A mirroring transform turns triangles inside out: wind them back.
        const bool isMirrored = glm::determinant(glm::mat3(transform)) < 0.0f;
        const size_t indexCount = primitive.indices != nullptr ? primitive.indices->count : count;
        const size_t firstIndex = mesh.Indices.size();
        for (size_t i = 0; i + 2 < indexCount; i += 3)
        {
            std::array<size_t, 3> corner{i, i + 1, i + 2};
            if (primitive.indices != nullptr)
            {
                for (size_t& index : corner)
                {
                    index = cgltf_accessor_read_index(primitive.indices, index);
                }
            }
            if (std::ranges::any_of(corner, [count](size_t index) { return index >= count; }))
            {
                continue;
            }
            if (isMirrored)
            {
                std::swap(corner[1], corner[2]);
            }
            for (const size_t index : corner)
            {
                mesh.Indices.push_back(first + static_cast<uint32_t>(index));
            }
        }

        // No normals in the file: each vertex takes the faces' around it, weighted by their size.
        if (normals == nullptr)
        {
            for (size_t i = first; i < mesh.Vertices.size(); ++i)
            {
                mesh.Vertices[i].Normal = glm::vec3(0.0f);
            }
            for (size_t i = firstIndex; i + 2 < mesh.Indices.size(); i += 3)
            {
                MeshVertex& a = mesh.Vertices[mesh.Indices[i]];
                MeshVertex& b = mesh.Vertices[mesh.Indices[i + 1]];
                MeshVertex& c = mesh.Vertices[mesh.Indices[i + 2]];
                const glm::vec3 face = glm::cross(b.Position - a.Position, c.Position - a.Position);
                a.Normal += face;
                b.Normal += face;
                c.Normal += face;
            }
            for (size_t i = first; i < mesh.Vertices.size(); ++i)
            {
                glm::vec3& normal = mesh.Vertices[i].Normal;
                normal = glm::length(normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
    }

    std::string path;
    ModelData model;
    std::map<const cgltf_material*, size_t> parts;
    size_t skipped = 0;
    bool hasWarnedOfSkin = false;
};

} // namespace

Result<ModelData> ReadModel(std::span<const std::byte> data, const std::string& path)
{
    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    if (cgltf_parse(&options, data.data(), data.size(), &parsed) != cgltf_result_success)
    {
        return Error{"Not a glTF model"};
    }
    const DataPointer model(parsed);
    for (size_t i = 0; i < model->buffers_count; ++i)
    {
        const char* uri = model->buffers[i].uri;
        if (uri != nullptr && !std::string_view(uri).starts_with("data:") && path.empty())
        {
            return Error{"The model's data is in another file: export it as .glb"};
        }
    }
    if (cgltf_load_buffers(&options, model.get(), path.empty() ? nullptr : path.c_str()) != cgltf_result_success)
    {
        return Error{"The model's data didn't load"};
    }
    if (cgltf_validate(model.get()) != cgltf_result_success)
    {
        return Error{"The model is malformed"};
    }

    ModelReader reader(path);
    const cgltf_scene* scene =
        model->scene != nullptr ? model->scene : (model->scenes_count > 0 ? model->scenes : nullptr);
    if (scene != nullptr)
    {
        for (size_t i = 0; i < scene->nodes_count; ++i)
        {
            reader.AddNode(*scene->nodes[i]);
        }
    }
    else
    {
        for (size_t i = 0; i < model->nodes_count; ++i)
        {
            if (model->nodes[i].parent == nullptr)
            {
                reader.AddNode(model->nodes[i]);
            }
        }
    }
    ModelData result = reader.Finish();
    if (result.Parts.empty())
    {
        return Error{"The model has no triangles"};
    }
    return result;
}

Result<ModelData> LoadModel(const std::string& path)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    if (bytes.empty())
    {
        return Error{"Couldn't read " + path};
    }
    return ReadModel(bytes, path);
}

} // namespace hyoshi::renderer
