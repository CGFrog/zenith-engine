#define CGLTF_IMPLEMENTATION
#include "ModelImporter.hpp"
#include "CompressedTexture.hpp"
#include "ImageDecoder.hpp"
#include "MeshOptimization.hpp"

#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <memory>
#include <string_view>

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

const cgltf_accessor* findAttribute(const cgltf_morph_target& target, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < target.attributes_count; ++i) {
        if (target.attributes[i].type == type && target.attributes[i].index == 0) {
            return target.attributes[i].data;
        }
    }
    return nullptr;
}

constexpr cgltf_size maximumVertices = 4'000'000;
constexpr cgltf_size maximumIndices = 12'000'000;
constexpr cgltf_size maximumMorphTargets = 64;
constexpr cgltf_size maximumTextureBytes = 512ull * 1024ull * 1024ull;

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

BoundingSphere calculateBounds(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        return {};
    }
    glm::vec3 minimum = vertices.front().position;
    glm::vec3 maximum = minimum;
    for (const Vertex& vertex : vertices) {
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }
    const glm::vec3 center = (minimum + maximum) * 0.5f;
    float radiusSquared = 0.0f;
    for (const Vertex& vertex : vertices) {
        const glm::vec3 offset = vertex.position - center;
        radiusSquared = std::max(radiusSquared, glm::dot(offset, offset));
    }
    return {.center = center, .radius = std::sqrt(radiusSquared)};
}

struct ImageUsage {
    TextureUsage usage{TextureUsage::Color};
    // False when the image's references disagree in a way one cooked format
    // cannot serve (a normal map also used as color or data), so every slot
    // keeps the full RGBA8 source.
    bool cookable{true};
};

// Derives each image's channel meaning from the material slots referencing it.
// Color wins over data because BC7 serves both, selecting sRGB per slot.
std::vector<ImageUsage> imageUsages(const cgltf_data& data) {
    constexpr uint32_t colorBit = 1u;
    constexpr uint32_t normalBit = 2u;
    constexpr uint32_t dataBit = 4u;
    std::vector<uint32_t> references(data.images_count, 0u);
    const auto reference = [&](const cgltf_texture_view& view, uint32_t usageBit) {
        if (!view.texture || !view.texture->image) {
            return;
        }
        const cgltf_size index = static_cast<cgltf_size>(view.texture->image - data.images);
        if (index < references.size()) {
            references[index] |= usageBit;
        }
    };
    for (cgltf_size materialIndex = 0; materialIndex < data.materials_count; ++materialIndex) {
        const cgltf_material& material = data.materials[materialIndex];
        if (material.has_pbr_metallic_roughness) {
            reference(material.pbr_metallic_roughness.base_color_texture, colorBit);
            reference(material.pbr_metallic_roughness.metallic_roughness_texture, dataBit);
        }
        reference(material.emissive_texture, colorBit);
        reference(material.normal_texture, normalBit);
        reference(material.occlusion_texture, dataBit);
        if (material.has_clearcoat) {
            reference(material.clearcoat.clearcoat_texture, dataBit);
            reference(material.clearcoat.clearcoat_roughness_texture, dataBit);
            reference(material.clearcoat.clearcoat_normal_texture, normalBit);
        }
        if (material.has_sheen) {
            reference(material.sheen.sheen_color_texture, colorBit);
            reference(material.sheen.sheen_roughness_texture, dataBit);
        }
        if (material.has_specular) {
            reference(material.specular.specular_texture, dataBit);
            reference(material.specular.specular_color_texture, colorBit);
        }
        if (material.has_transmission) {
            reference(material.transmission.transmission_texture, dataBit);
        }
    }
    std::vector<ImageUsage> usages(data.images_count);
    for (size_t index = 0; index < references.size(); ++index) {
        const uint32_t used = references[index];
        if (used == normalBit) {
            usages[index].usage = TextureUsage::Normal;
        } else if (used == dataBit) {
            usages[index].usage = TextureUsage::Data;
        }
        usages[index].cookable = (used & normalBit) == 0 || used == normalBit;
    }
    return usages;
}

// Bytes encoded by base64 text (a data URI payload): three per four
// characters, minus one per trailing '=' padding character. Zero for text
// whose length is not a whole number of four-character groups.
size_t base64DecodedSize(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return 0;
    }
    size_t padding = 0;
    while (padding < 2 && encoded[encoded.size() - 1 - padding] == '=') {
        ++padding;
    }
    return encoded.size() / 4 * 3 - padding;
}

bool isExternalImageFile(const cgltf_image& image) {
    return !image.buffer_view && image.uri && std::strncmp(image.uri, "data:", 5) != 0;
}

// Prefers a cooked block-compressed texture the renderer can sample, falling
// back to decoding the source image.
ImageDecodeResult decodeExternalImage(const AssetManager& assets, const std::filesystem::path& source,
                                      TextureUsage usage) {
    if (!assets.cookedTextureRoot().empty() &&
        textureFormatSupported(cookedTextureFormat(usage), assets.textureFormatCapabilities())) {
        CookedTextureLookup cooked = loadCookedTexture(assets.cookedTextureRoot(), source, usage);
        if (cooked.texture) {
            return {.texture = std::move(*cooked.texture)};
        }
        if (!cooked.error.empty()) {
            assets.log().warning(cooked.error + "; decoding the source image instead");
        }
    }
    return decodeImageFile(source);
}

TextureHandle textureHandle(const cgltf_texture_view& view, const cgltf_data& data,
                            const std::vector<TextureHandle>& images) {
    if (!view.texture || !view.texture->image) {
        return {};
    }
    const cgltf_size index = static_cast<cgltf_size>(view.texture->image - data.images);
    return index < images.size() ? images[index] : TextureHandle{};
}

// Reads the KHR material extensions layered over metallic-roughness; absent
// extensions keep MaterialAsset's defaults, which leave shading unchanged.
void importMaterialExtensions(const cgltf_material& source, const cgltf_data& data,
                              const std::vector<TextureHandle>& images, MaterialAsset& material) {
    if (source.has_ior) {
        material.ior = source.ior.ior;
    }
    if (source.has_clearcoat) {
        const cgltf_clearcoat& clearcoat = source.clearcoat;
        material.clearcoat.factor = clearcoat.clearcoat_factor;
        material.clearcoat.roughness = clearcoat.clearcoat_roughness_factor;
        material.clearcoat.normalScale = clearcoat.clearcoat_normal_texture.scale;
        material.clearcoat.texture = textureHandle(clearcoat.clearcoat_texture, data, images);
        material.clearcoat.roughnessTexture = textureHandle(clearcoat.clearcoat_roughness_texture, data, images);
        material.clearcoat.normalTexture = textureHandle(clearcoat.clearcoat_normal_texture, data, images);
    }
    if (source.has_sheen) {
        const cgltf_sheen& sheen = source.sheen;
        material.sheen.color = {sheen.sheen_color_factor[0], sheen.sheen_color_factor[1], sheen.sheen_color_factor[2]};
        material.sheen.roughness = sheen.sheen_roughness_factor;
        material.sheen.colorTexture = textureHandle(sheen.sheen_color_texture, data, images);
        material.sheen.roughnessTexture = textureHandle(sheen.sheen_roughness_texture, data, images);
    }
    if (source.has_specular) {
        const cgltf_specular& specular = source.specular;
        material.specular.factor = specular.specular_factor;
        material.specular.color = {specular.specular_color_factor[0], specular.specular_color_factor[1],
                                   specular.specular_color_factor[2]};
        material.specular.texture = textureHandle(specular.specular_texture, data, images);
        material.specular.colorTexture = textureHandle(specular.specular_color_texture, data, images);
    }
    if (source.has_transmission) {
        material.transmission.factor = source.transmission.transmission_factor;
        material.transmission.texture = textureHandle(source.transmission.transmission_texture, data, images);
    }
    if (source.has_volume) {
        const cgltf_volume& volume = source.volume;
        material.transmission.thickness = volume.thickness_factor;
        material.transmission.attenuationColor = {volume.attenuation_color[0], volume.attenuation_color[1],
                                                  volume.attenuation_color[2]};
        // cgltf reports a missing distance as the largest float; zero is
        // invalid glTF. Both mean a medium that does not absorb.
        const bool absorbs =
            volume.attenuation_distance > 0.0f && volume.attenuation_distance < std::numeric_limits<float>::max();
        material.transmission.attenuationDistance =
            absorbs ? volume.attenuation_distance : std::numeric_limits<float>::infinity();
    }
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
    for (cgltf_size viewIndex = 0; viewIndex < data->buffer_views_count; ++viewIndex) {
        if (data->buffer_views[viewIndex].has_meshopt_compression) {
            return {.error = "Meshopt-compressed glTF requires the optional Meshopt decoder: " + path.string()};
        }
    }
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex) {
        for (cgltf_size primitiveIndex = 0; primitiveIndex < data->meshes[meshIndex].primitives_count;
             ++primitiveIndex) {
            if (data->meshes[meshIndex].primitives[primitiveIndex].has_draco_mesh_compression) {
                return {.error = "Draco-compressed glTF requires the optional Draco decoder: " + path.string()};
            }
        }
    }
    result = cgltf_load_buffers(&options, data.get(), path.string().c_str());

    if (result != cgltf_result_success) {
        return {.error = "Cannot load buffers for " + path.string() + ": " + resultMessage(result)};
    }

    result = cgltf_validate(data.get());

    if (result != cgltf_result_success) {
        return {.error = "Invalid glTF " + path.string() + ": " + resultMessage(result)};
    }

    const std::vector<ImageUsage> usages = imageUsages(*data);
    std::vector<TextureHandle> images(data->images_count);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& image = data->images[i];
        ImageDecodeResult decoded;
        if (image.buffer_view && image.buffer_view->buffer && image.buffer_view->buffer->data) {
            if (image.buffer_view->size > maximumTextureBytes) {
                return {.error = "Embedded texture exceeds the 512 MiB decoded-input limit: " + path.string()};
            }
            const auto* bytes =
                static_cast<const uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
            decoded =
                decodeImageMemory(bytes, image.buffer_view->size, image.name ? image.name : "embedded glTF image");
        } else if (isExternalImageFile(image)) {
            decoded = usages[i].cookable
                          ? decodeExternalImage(m_assets, path.parent_path() / image.uri, usages[i].usage)
                          : decodeImageFile(path.parent_path() / image.uri);
        } else if (image.uri) {
            const char* comma = std::strchr(image.uri, ',');
            const size_t decodedSize = comma ? base64DecodedSize(comma + 1) : 0;
            void* bytes = nullptr;
            // cgltf decodes exactly decodedSize bytes, so it must be the decoded
            // length, not the length of the encoded text.
            if (decodedSize > 0 &&
                cgltf_load_buffer_base64(&options, decodedSize, comma + 1, &bytes) == cgltf_result_success) {
                decoded = decodeImageMemory(bytes, decodedSize, image.name ? image.name : "data URI image");
                std::free(bytes);
            } else {
                decoded.error = "Cannot decode data URI image in " + path.string();
            }
        }
        if (!decoded) {
            return {.error = decoded.error};
        }
        decoded.texture.usage = usages[i].usage;
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
        material.alphaMode = source.alpha_mode == cgltf_alpha_mode_mask    ? MaterialAlphaMode::Mask
                             : source.alpha_mode == cgltf_alpha_mode_blend ? MaterialAlphaMode::Blend
                                                                           : MaterialAlphaMode::Opaque;
        material.alphaCutoff = source.alpha_cutoff;
        material.doubleSided = source.double_sided != 0;
        importMaterialExtensions(source, *data, images, material);

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
                m_assets.log().warning("Skipping non-triangle primitive in " + path.string());
                continue;
            }

            const cgltf_accessor* positions = findAttribute(primitive, cgltf_attribute_type_position);
            const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
            const cgltf_accessor* texCoords = findAttribute(primitive, cgltf_attribute_type_texcoord);
            const cgltf_accessor* sourceTangents = findAttribute(primitive, cgltf_attribute_type_tangent);
            const cgltf_accessor* sourceJoints = findAttribute(primitive, cgltf_attribute_type_joints);
            const cgltf_accessor* sourceWeights = findAttribute(primitive, cgltf_attribute_type_weights);

            if (!positions || positions->type != cgltf_type_vec3) {
                m_assets.log().warning("Skipping primitive without POSITION vec3 in " + path.string());
                continue;
            }
            if (positions->count > maximumVertices || primitive.targets_count > maximumMorphTargets) {
                return {.error = "glTF primitive exceeds vertex or morph-target safety limits: " + path.string()};
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
                    mesh.vertices[firstVertex + vertexIndex].tangent = {tangent[0], tangent[1], tangent[2], tangent[3]};
                }
                if (sourceJoints && sourceJoints->type == cgltf_type_vec4 && vertexIndex < sourceJoints->count) {
                    cgltf_uint joints[4]{};
                    cgltf_accessor_read_uint(sourceJoints, vertexIndex, joints, 4);
                    mesh.vertices[firstVertex + vertexIndex].joints = {joints[0], joints[1], joints[2], joints[3]};
                }
                if (sourceWeights && sourceWeights->type == cgltf_type_vec4 && vertexIndex < sourceWeights->count) {
                    float weights[4]{};
                    cgltf_accessor_read_float(sourceWeights, vertexIndex, weights, 4);
                    glm::vec4 normalized{weights[0], weights[1], weights[2], weights[3]};
                    const float total = normalized.x + normalized.y + normalized.z + normalized.w;
                    mesh.vertices[firstVertex + vertexIndex].weights =
                        total > 0.0f ? normalized / total : glm::vec4(0.0f);
                }
            }

            const cgltf_size count = primitive.indices ? primitive.indices->count : positions->count;
            if (count > maximumIndices) {
                return {.error = "glTF primitive exceeds the index safety limit: " + path.string()};
            }

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

            if (mesh.morphTargets.size() < primitive.targets_count) {
                mesh.morphTargets.resize(primitive.targets_count);
            }
            for (cgltf_size targetIndex = 0; targetIndex < primitive.targets_count; ++targetIndex) {
                MorphTarget& target = mesh.morphTargets[targetIndex];
                target.name = targetIndex < sourceMesh.target_names_count && sourceMesh.target_names[targetIndex]
                                  ? sourceMesh.target_names[targetIndex]
                                  : "Morph " + std::to_string(targetIndex);
                target.positionDeltas.resize(mesh.vertices.size());
                target.normalDeltas.resize(mesh.vertices.size());
                target.tangentDeltas.resize(mesh.vertices.size());
                const cgltf_morph_target& sourceTarget = primitive.targets[targetIndex];
                const cgltf_accessor* positionDeltas = findAttribute(sourceTarget, cgltf_attribute_type_position);
                const cgltf_accessor* normalDeltas = findAttribute(sourceTarget, cgltf_attribute_type_normal);
                const cgltf_accessor* tangentDeltas = findAttribute(sourceTarget, cgltf_attribute_type_tangent);
                for (cgltf_size vertexIndex = 0; vertexIndex < positions->count; ++vertexIndex) {
                    float value[3]{};
                    if (positionDeltas && vertexIndex < positionDeltas->count) {
                        cgltf_accessor_read_float(positionDeltas, vertexIndex, value, 3);
                        target.positionDeltas[firstVertex + vertexIndex] = {value[0], value[1], value[2]};
                    }
                    if (normalDeltas && vertexIndex < normalDeltas->count) {
                        cgltf_accessor_read_float(normalDeltas, vertexIndex, value, 3);
                        target.normalDeltas[firstVertex + vertexIndex] = {value[0], value[1], value[2]};
                    }
                    if (tangentDeltas && vertexIndex < tangentDeltas->count) {
                        cgltf_accessor_read_float(tangentDeltas, vertexIndex, value, 3);
                        target.tangentDeltas[firstVertex + vertexIndex] = {value[0], value[1], value[2]};
                    }
                }
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
            optimizeMesh(mesh);
            mesh.bounds = calculateBounds(mesh.vertices);
            meshes[meshIndex] = m_assets.addMesh(std::move(mesh), path, static_cast<uint32_t>(meshIndex));
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
        node.translation = source.has_translation
                               ? glm::vec3(source.translation[0], source.translation[1], source.translation[2])
                               : glm::vec3(0.0f);
        node.rotation = source.has_rotation ? glm::normalize(glm::quat(source.rotation[3], source.rotation[0],
                                                                       source.rotation[1], source.rotation[2]))
                                            : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        node.scale = source.has_scale ? glm::vec3(source.scale[0], source.scale[1], source.scale[2]) : glm::vec3(1.0f);

        if (source.parent) {
            node.parent = static_cast<int32_t>(source.parent - data->nodes);
        }

        if (source.mesh) {
            const cgltf_size meshIndex = static_cast<cgltf_size>(source.mesh - data->meshes);

            if (meshIndex < meshes.size() && meshes[meshIndex]) {
                node.mesh = meshes[meshIndex];
                renderable = true;
                const cgltf_float* weights = source.weights_count > 0 ? source.weights : source.mesh->weights;
                const cgltf_size weightCount =
                    source.weights_count > 0 ? source.weights_count : source.mesh->weights_count;
                if (weightCount > 0 && weights != nullptr) {
                    node.morphWeights.assign(weights, weights + weightCount);
                }
            }
        }
        if (source.skin) {
            node.skin = static_cast<int32_t>(source.skin - data->skins);
        }
    }

    model.skins.resize(data->skins_count);
    for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex) {
        const cgltf_skin& source = data->skins[skinIndex];
        SkinAsset& skin = model.skins[skinIndex];
        skin.name = source.name ? source.name : "Skin";
        skin.skeletonRoot = source.skeleton ? static_cast<int32_t>(source.skeleton - data->nodes) : -1;
        skin.joints.reserve(source.joints_count);
        skin.inverseBindMatrices.assign(source.joints_count, glm::mat4(1.0f));
        for (cgltf_size jointIndex = 0; jointIndex < source.joints_count; ++jointIndex) {
            skin.joints.push_back(static_cast<uint32_t>(source.joints[jointIndex] - data->nodes));
            if (source.inverse_bind_matrices && jointIndex < source.inverse_bind_matrices->count) {
                float inverseBind[16]{};
                if (!cgltf_accessor_read_float(source.inverse_bind_matrices, jointIndex, inverseBind, 16)) {
                    return {.error = "Cannot read inverse-bind matrix in " + path.string()};
                }
                skin.inverseBindMatrices[jointIndex] = glm::make_mat4(inverseBind);
            }
        }
    }

    model.animations.reserve(data->animations_count);
    for (cgltf_size animationIndex = 0; animationIndex < data->animations_count; ++animationIndex) {
        const cgltf_animation& sourceAnimation = data->animations[animationIndex];
        AnimationClip clip;
        clip.name = sourceAnimation.name ? sourceAnimation.name : "Animation " + std::to_string(animationIndex);
        for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation.channels_count; ++channelIndex) {
            const cgltf_animation_channel& sourceChannel = sourceAnimation.channels[channelIndex];
            if (!sourceChannel.target_node || !sourceChannel.sampler || !sourceChannel.sampler->input ||
                !sourceChannel.sampler->output) {
                continue;
            }
            AnimationChannel channel;
            channel.targetNode = static_cast<uint32_t>(sourceChannel.target_node - data->nodes);
            channel.path = sourceChannel.target_path == cgltf_animation_path_type_rotation ? AnimationPath::Rotation
                           : sourceChannel.target_path == cgltf_animation_path_type_scale  ? AnimationPath::Scale
                           : sourceChannel.target_path == cgltf_animation_path_type_weights
                               ? AnimationPath::Weights
                               : AnimationPath::Translation;
            channel.interpolation = sourceChannel.sampler->interpolation == cgltf_interpolation_type_step
                                        ? AnimationInterpolation::Step
                                    : sourceChannel.sampler->interpolation == cgltf_interpolation_type_cubic_spline
                                        ? AnimationInterpolation::CubicSpline
                                        : AnimationInterpolation::Linear;
            const cgltf_accessor& input = *sourceChannel.sampler->input;
            const cgltf_accessor& output = *sourceChannel.sampler->output;
            if (input.type != cgltf_type_scalar || input.count == 0) {
                continue;
            }
            const cgltf_size components =
                channel.path == AnimationPath::Weights
                    ? (sourceChannel.target_node->mesh && sourceChannel.target_node->mesh->primitives_count > 0
                           ? sourceChannel.target_node->mesh->primitives[0].targets_count
                           : 0)
                : channel.path == AnimationPath::Rotation ? 4
                                                          : 3;
            if (components == 0 || components > maximumMorphTargets) {
                return {.error = "Animation weight channel exceeds morph-target safety limits: " + path.string()};
            }
            channel.times.resize(input.count);
            if (channel.path == AnimationPath::Weights) {
                channel.weightValues.assign(input.count, std::vector<float>(components));
                if (channel.interpolation == AnimationInterpolation::CubicSpline) {
                    channel.weightInTangents.assign(input.count, std::vector<float>(components));
                    channel.weightOutTangents.assign(input.count, std::vector<float>(components));
                }
            } else {
                channel.values.resize(input.count);
                if (channel.interpolation == AnimationInterpolation::CubicSpline) {
                    channel.inTangents.resize(input.count);
                    channel.outTangents.resize(input.count);
                }
            }
            for (cgltf_size key = 0; key < input.count; ++key) {
                cgltf_accessor_read_float(&input, key, &channel.times[key], 1);
                clip.duration = std::max(clip.duration, channel.times[key]);
                const cgltf_size valueIndex =
                    channel.interpolation == AnimationInterpolation::CubicSpline ? key * 3 + 1 : key;
                std::vector<float> value(components);
                const cgltf_size flatValueIndex =
                    channel.path == AnimationPath::Weights ? valueIndex * components : valueIndex;
                if (flatValueIndex >= output.count ||
                    (channel.path == AnimationPath::Weights
                         ? !cgltf_accessor_read_float(&output, flatValueIndex, value.data(), components)
                         : !cgltf_accessor_read_float(&output, valueIndex, value.data(), components))) {
                    return {.error = "Invalid animation output in " + path.string()};
                }
                if (channel.path == AnimationPath::Weights) {
                    channel.weightValues[key] = value;
                } else {
                    channel.values[key] = {value[0], value[1], value[2], components == 4 ? value[3] : 0.0f};
                }
                if (channel.interpolation == AnimationInterpolation::CubicSpline) {
                    std::vector<float> inTangent(components);
                    std::vector<float> outTangent(components);
                    const cgltf_size tangentBase =
                        channel.path == AnimationPath::Weights ? key * 3 * components : key * 3;
                    if (!cgltf_accessor_read_float(&output, tangentBase, inTangent.data(), components) ||
                        !cgltf_accessor_read_float(
                            &output, tangentBase + (channel.path == AnimationPath::Weights ? 2 * components : 2),
                            outTangent.data(), components)) {
                        return {.error = "Invalid cubic animation tangents in " + path.string()};
                    }
                    if (channel.path == AnimationPath::Weights) {
                        channel.weightInTangents[key] = std::move(inTangent);
                        channel.weightOutTangents[key] = std::move(outTangent);
                    } else {
                        channel.inTangents[key] = {inTangent[0], inTangent[1], inTangent[2],
                                                   components == 4 ? inTangent[3] : 0.0f};
                        channel.outTangents[key] = {outTangent[0], outTangent[1], outTangent[2],
                                                    components == 4 ? outTangent[3] : 0.0f};
                    }
                }
            }
            clip.channels.push_back(std::move(channel));
        }
        model.animations.push_back(std::move(clip));
    }

    if (!renderable) {
        return {.error = "Model contains no renderable triangle primitives: " + path.string()};
    }

    return {.handle = m_assets.addModel(std::move(model), path)};
}

ModelTextureSourcesResult collectModelTextureSources(const std::filesystem::path& modelPath) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    const cgltf_result result = cgltf_parse_file(&options, modelPath.string().c_str(), &raw);
    if (result != cgltf_result_success) {
        return {.error = "Cannot parse " + modelPath.string() + ": " + resultMessage(result)};
    }
    std::unique_ptr<cgltf_data, CgltfDeleter> data(raw);
    const std::vector<ImageUsage> usages = imageUsages(*data);
    ModelTextureSourcesResult collected;
    for (cgltf_size index = 0; index < data->images_count; ++index) {
        if (isExternalImageFile(data->images[index]) && usages[index].cookable) {
            collected.sources.push_back({modelPath.parent_path() / data->images[index].uri, usages[index].usage});
        }
    }
    return collected;
}
