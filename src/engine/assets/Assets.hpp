#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

template <typename Tag> struct AssetHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    uint32_t generation{0};

    explicit operator bool() const {
        return index != std::numeric_limits<uint32_t>::max();
    }

    auto operator<=>(const AssetHandle&) const = default;
};

struct ModelTag;
struct MeshTag;
struct MaterialTag;
struct TextureTag;

using ModelHandle = AssetHandle<ModelTag>;
using MeshHandle = AssetHandle<MeshTag>;
using MaterialHandle = AssetHandle<MaterialTag>;
using TextureHandle = AssetHandle<TextureTag>;

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 texCoord{};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

struct TextureAsset {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;
};

struct MaterialAsset {
    glm::vec4 baseColor{1.0f};
    glm::vec3 emissive{0.0f};
    float emissiveStrength{1.0f};
    float metallic{1.0f};
    float roughness{1.0f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    TextureHandle baseColorTexture{};
    TextureHandle normalTexture{};
    TextureHandle metallicRoughnessTexture{};
    TextureHandle occlusionTexture{};
    TextureHandle emissiveTexture{};
};

struct MeshPrimitive {
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    MaterialHandle material{};
};

struct MeshAsset {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitive> primitives;
};

struct ModelNode {
    std::string name;
    glm::mat4 localTransform{1.0f};
    int32_t parent{-1};
    MeshHandle mesh{};
};

struct ModelAsset {
    std::vector<ModelNode> nodes;
};

template <typename Handle> struct AssetLoadResult {
    Handle handle{};
    std::string error;

    explicit operator bool() const {
        return static_cast<bool>(handle);
    }
};

class AssetManager {
  public:
    AssetLoadResult<ModelHandle> loadModel(const std::filesystem::path& path);
    const ModelAsset* model(ModelHandle handle) const;
    const MeshAsset* mesh(MeshHandle handle) const;
    const MaterialAsset* material(MaterialHandle handle) const;
    const TextureAsset* texture(TextureHandle handle) const;

  private:
    friend class ModelImporter;
    ModelHandle addModel(ModelAsset asset);
    MeshHandle addMesh(MeshAsset asset);
    MaterialHandle addMaterial(MaterialAsset asset);
    TextureHandle addTexture(TextureAsset asset);

    std::vector<ModelAsset> m_models;
    std::vector<MeshAsset> m_meshes;
    std::vector<MaterialAsset> m_materials;
    std::vector<TextureAsset> m_textures;
    std::unordered_map<std::string, ModelHandle> m_modelCache;
};
