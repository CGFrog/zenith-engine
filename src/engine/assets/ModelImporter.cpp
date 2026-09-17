#define CGLTF_IMPLEMENTATION
#include "ModelImporter.hpp"
#include "ImageDecoder.hpp"

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <memory>
#include <cstdlib>

namespace {
struct CgltfDeleter {
    void operator()(cgltf_data* data) const {
        cgltf_free(data);
    }
};

const char* resultMessage(cgltf_result result) {
    switch (result) {
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "legacy glTF is unsupported";
    default:
        return "unknown cgltf error";
    }
}

const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
            return primitive.attributes[i].data;
        }
    }

    return nullptr;
}

void generateNormals(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex) {
    for (uint32_t i = 0; i < vertexCount; ++i) {
        mesh.vertices[firstVertex + i].normal = glm::vec3(0.0f);
    }

    for (uint32_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c = mesh.indices[i + 2];
        const glm::vec3 normal = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                            mesh.vertices[c].position - mesh.vertices[a].position);

        mesh.vertices[a].normal += normal;
        mesh.vertices[b].normal += normal;
        mesh.vertices[c].normal += normal;
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        glm::vec3& normal = mesh.vertices[firstVertex + i].normal;
        normal = glm::dot(normal, normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

void generateTangents(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex) {
    std::vector<glm::vec3> tangents(vertexCount);
    std::vector<glm::vec3> bitangents(vertexCount);
    for (uint32_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t ia = mesh.indices[i];
        const uint32_t ib = mesh.indices[i + 1];
        const uint32_t ic = mesh.indices[i + 2];
        const Vertex& a = mesh.vertices[ia];
        const Vertex& b = mesh.vertices[ib];
        const Vertex& c = mesh.vertices[ic];
        const glm::vec3 edge1 = b.position - a.position;
        const glm::vec3 edge2 = c.position - a.position;
        const glm::vec2 uv1 = b.texCoord - a.texCoord;
        const glm::vec2 uv2 = c.texCoord - a.texCoord;
        const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
        if (std::abs(determinant) < 1.0e-8f) {
            continue;
        }
        const float inverse = 1.0f / determinant;
        const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * inverse;
        const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * inverse;
        for (uint32_t index : {ia, ib, ic}) {
            tangents[index - firstVertex] += tangent;
            bitangents[index - firstVertex] += bitangent;
        }
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 normal = mesh.vertices[firstVertex + i].normal;
        glm::vec3 tangent = tangents[i] - normal * glm::dot(normal, tangents[i]);
        if (glm::dot(tangent, tangent) < 1.0e-8f) {
            tangent = std::abs(normal.y) < 0.999f ? glm::normalize(glm::cross({0.0f, 1.0f, 0.0f}, normal))
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            tangent = glm::normalize(tangent);
        }
        const float handedness = glm::dot(glm::cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        mesh.vertices[firstVertex + i].tangent = {tangent, handedness};
    }
}

TextureHandle textureHandle(const cgltf_texture_view& view, const cgltf_data& data,
                            const std::vector<TextureHandle>& images) {
    if (!view.texture || !view.texture->image) {
        return {};
    }
    const cgltf_size index = static_cast<cgltf_size>(view.texture->image - data.images);
    return index < images.size() ? images[index] : TextureHandle{};
}
} // namespace

AssetLoadResult<ModelHandle> ModelImporter::load(const std::filesystem::path& path) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &raw);
    if (result != cgltf_result_success) {
        return {.error = "Cannot parse " + path.string() + ": " + resultMessage(result)};
    }

    std::unique_ptr<cgltf_data, CgltfDeleter> data(raw);
    result = cgltf_load_buffers(&options, data.get(), path.string().c_str());

    if (result != cgltf_result_success) {
        return {.error = "Cannot load buffers for " + path.string() + ": " + resultMessage(result)};
    }

    result = cgltf_validate(data.get());

    if (result != cgltf_result_success) {
        return {.error = "Invalid glTF " + path.string() + ": " + resultMessage(result)};
    }

    std::vector<TextureHandle> images(data->images_count);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& image = data->images[i];
        ImageDecodeResult decoded;
        if (image.buffer_view && image.buffer_view->buffer && image.buffer_view->buffer->data) {
            const auto* bytes = static_cast<const uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
            decoded = decodeImageMemory(bytes, image.buffer_view->size, image.name ? image.name : "embedded glTF image");
        } else if (image.uri && std::strncmp(image.uri, "data:", 5) != 0) {
            decoded = decodeImageFile(path.parent_path() / image.uri);
        } else if (image.uri) {
            const char* comma = std::strchr(image.uri, ',');
            void* bytes = nullptr;
            if (comma && cgltf_load_buffer_base64(&options, std::strlen(comma + 1), comma + 1, &bytes) ==
                             cgltf_result_success) {
                const size_t decodedSize = (std::strlen(comma + 1) / 4) * 3;
                decoded = decodeImageMemory(bytes, decodedSize, image.name ? image.name : "data URI image");
                std::free(bytes);
            } else {
                decoded.error = "Cannot decode data URI image in " + path.string();
            }
        }
        if (!decoded) {
            return {.error = decoded.error};
        }
        images[i] = m_assets.addTexture(std::move(decoded.texture));
    }

    std::vector<MaterialHandle> materials(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        MaterialAsset material{};
        const cgltf_material& source = data->materials[i];
        if (source.has_pbr_metallic_roughness) {
            const auto& pbr = source.pbr_metallic_roughness;
            const float* factor = pbr.base_color_factor;
            material.baseColor = {factor[0], factor[1], factor[2], factor[3]};
            material.metallic = pbr.metallic_factor;
            material.roughness = pbr.roughness_factor;
            material.baseColorTexture = textureHandle(pbr.base_color_texture, *data, images);
            material.metallicRoughnessTexture = textureHandle(pbr.metallic_roughness_texture, *data, images);
        }
        material.emissive = {source.emissive_factor[0], source.emissive_factor[1], source.emissive_factor[2]};
        material.emissiveStrength = source.has_emissive_strength ? source.emissive_strength.emissive_strength : 1.0f;
        material.normalScale = source.normal_texture.scale;
        material.occlusionStrength = source.occlusion_texture.scale;
        material.normalTexture = textureHandle(source.normal_texture, *data, images);
        material.occlusionTexture = textureHandle(source.occlusion_texture, *data, images);
        material.emissiveTexture = textureHandle(source.emissive_texture, *data, images);

        materials[i] = m_assets.addMaterial(material);
    }
    const MaterialHandle defaultMaterial = m_assets.addMaterial({});

    std::vector<MeshHandle> meshes(data->meshes_count);
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex) {
        MeshAsset mesh;
        const cgltf_mesh& sourceMesh = data->meshes[meshIndex];

        for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex) {
            const cgltf_primitive& primitive = sourceMesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles) {
                std::cerr << "Skipping non-triangle primitive in " << path << '\n';
                continue;
            }

            const cgltf_accessor* positions = findAttribute(primitive, cgltf_attribute_type_position);
            const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
            const cgltf_accessor* texCoords = findAttribute(primitive, cgltf_attribute_type_texcoord);
            const cgltf_accessor* sourceTangents = findAttribute(primitive, cgltf_attribute_type_tangent);

            if (!positions || positions->type != cgltf_type_vec3) {
                std::cerr << "Skipping primitive without POSITION vec3 in " << path << '\n';
                continue;
            }

            const uint32_t firstVertex = static_cast<uint32_t>(mesh.vertices.size());
            const uint32_t vertexCount = static_cast<uint32_t>(positions->count);
            const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
            mesh.vertices.resize(mesh.vertices.size() + vertexCount);

            for (cgltf_size vertexIndex = 0; vertexIndex < positions->count; ++vertexIndex) {
                float value[3]{};
                cgltf_accessor_read_float(positions, vertexIndex, value, 3);
                mesh.vertices[firstVertex + vertexIndex].position = {value[0], value[1], value[2]};

                if (normals && normals->type == cgltf_type_vec3 && vertexIndex < normals->count) {
                    cgltf_accessor_read_float(normals, vertexIndex, value, 3);
                    glm::vec3 normal{value[0], value[1], value[2]};
                    mesh.vertices[firstVertex + vertexIndex].normal =
                        glm::dot(normal, normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
                }
                if (texCoords && texCoords->type == cgltf_type_vec2 && vertexIndex < texCoords->count) {
                    float uv[2]{};
                    cgltf_accessor_read_float(texCoords, vertexIndex, uv, 2);
                    mesh.vertices[firstVertex + vertexIndex].texCoord = {uv[0], uv[1]};
                }
                if (sourceTangents && sourceTangents->type == cgltf_type_vec4 && vertexIndex < sourceTangents->count) {
                    float tangent[4]{};
                    cgltf_accessor_read_float(sourceTangents, vertexIndex, tangent, 4);
                    mesh.vertices[firstVertex + vertexIndex].tangent =
                        {tangent[0], tangent[1], tangent[2], tangent[3]};
                }
            }

            const cgltf_size count = primitive.indices ? primitive.indices->count : positions->count;

            for (cgltf_size i = 0; i < count; ++i) {
                const cgltf_size localIndex = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;

                if (localIndex >= positions->count) {
                    return {.error = "Out-of-range index in " + path.string()};
                }

                mesh.indices.push_back(firstVertex + static_cast<uint32_t>(localIndex));
            }

            if (!normals) {
                generateNormals(mesh, firstVertex, vertexCount, firstIndex);
            }
            if (texCoords && !sourceTangents) {
                generateTangents(mesh, firstVertex, vertexCount, firstIndex);
            }

            MaterialHandle material = defaultMaterial;

            if (primitive.material) {
                const cgltf_size i = static_cast<cgltf_size>(primitive.material - data->materials);

                if (i < materials.size()) {
                    material = materials[i];
                }
            }

            mesh.primitives.push_back({firstIndex, static_cast<uint32_t>(count), material});
        }

        if (!mesh.primitives.empty()) {
            meshes[meshIndex] = m_assets.addMesh(std::move(mesh));
        }
    }

    ModelAsset model;
    model.nodes.resize(data->nodes_count);
    bool renderable = false;
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        ModelNode& node = model.nodes[i];
        node.name = source.name ? source.name : "Node";
        float matrix[16]{};
        cgltf_node_transform_local(&source, matrix);
        node.localTransform = glm::make_mat4(matrix);

        if (source.parent) {
            node.parent = static_cast<int32_t>(source.parent - data->nodes);
        }

        if (source.mesh) {
            const cgltf_size meshIndex = static_cast<cgltf_size>(source.mesh - data->meshes);

            if (meshIndex < meshes.size() && meshes[meshIndex]) {
                node.mesh = meshes[meshIndex];
                renderable = true;
            }
        }
    }

    if (!renderable) {
        return {.error = "Model contains no renderable triangle primitives: " + path.string()};
    }

    return {.handle = m_assets.addModel(std::move(model))};
}
