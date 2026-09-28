#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <optional>
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
struct FontTag;

using ModelHandle = AssetHandle<ModelTag>;
using MeshHandle = AssetHandle<MeshTag>;
using MaterialHandle = AssetHandle<MaterialTag>;
using TextureHandle = AssetHandle<TextureTag>;
using FontHandle = AssetHandle<FontTag>;

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 texCoord{};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::uvec4 joints{0};
    glm::vec4 weights{0.0f};
};

struct TextureAsset {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;
};

struct FontAsset {
    std::vector<uint8_t> bytes;
};

enum class MaterialAlphaMode : uint8_t {
    Opaque,
    Mask,
    Blend,
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
    MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
    float alphaCutoff{0.5f};
    bool doubleSided{false};
};

struct MeshPrimitive {
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    MaterialHandle material{};
};

struct BoundingSphere {
    glm::vec3 center{0.0f};
    float radius{0.0f};
};

struct MeshAsset {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitive> primitives;
    BoundingSphere bounds;
};

struct ModelNode {
    std::string name;
    glm::mat4 localTransform{1.0f};
    int32_t parent{-1};
    MeshHandle mesh{};
    int32_t skin{-1};
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct SkinAsset {
    std::string name;
    std::vector<uint32_t> joints;
    std::vector<glm::mat4> inverseBindMatrices;
    int32_t skeletonRoot{-1};
};

enum class AnimationInterpolation : uint8_t {
    Step,
    Linear,
    CubicSpline,
};

enum class AnimationPath : uint8_t {
    Translation,
    Rotation,
    Scale,
};

struct AnimationChannel {
    uint32_t targetNode{0};
    AnimationPath path{AnimationPath::Translation};
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    std::vector<float> times;
    std::vector<glm::vec4> values;
    std::vector<glm::vec4> inTangents;
    std::vector<glm::vec4> outTangents;
};

struct AnimationClip {
    std::string name;
    float duration{0.0f};
    std::vector<AnimationChannel> channels;
};

struct ModelAsset {
    std::vector<ModelNode> nodes;
    std::vector<SkinAsset> skins;
    std::vector<AnimationClip> animations;
};

struct ModelAssetSource {
    std::filesystem::path path;
};

struct MeshAssetSource {
    std::filesystem::path path;
    uint32_t meshIndex{0};
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
    // Imports a fresh immutable model graph and updates future lookups while
    // retaining old handles for scenes and GPU frames that still use them.
    AssetLoadResult<ModelHandle> reloadModel(const std::filesystem::path& path);
    AssetLoadResult<MeshHandle> loadMesh(const std::filesystem::path& modelPath, uint32_t meshIndex);
    AssetLoadResult<FontHandle> loadFont(const std::filesystem::path& path);
    const ModelAsset* model(ModelHandle handle) const;
    const MeshAsset* mesh(MeshHandle handle) const;
    const MaterialAsset* material(MaterialHandle handle) const;
    const TextureAsset* texture(TextureHandle handle) const;
    const FontAsset* font(FontHandle handle) const;
    size_t fontCount() const;
    std::optional<ModelAssetSource> modelSource(ModelHandle handle) const;
    std::optional<MeshAssetSource> meshSource(MeshHandle handle) const;
    uint64_t revision() const;

  private:
    friend class ModelImporter;
    ModelHandle addModel(ModelAsset asset, std::filesystem::path sourcePath);
    MeshHandle addMesh(MeshAsset asset, std::filesystem::path sourcePath, uint32_t meshIndex);
    MaterialHandle addMaterial(MaterialAsset asset);
    TextureHandle addTexture(TextureAsset asset);
    AssetLoadResult<ModelHandle> importModel(const std::filesystem::path& path, const std::string& cacheKey);

    std::vector<ModelAsset> m_models;
    std::vector<ModelAssetSource> m_modelSources;
    std::vector<MeshAsset> m_meshes;
    std::vector<MeshAssetSource> m_meshSources;
    std::vector<MaterialAsset> m_materials;
    std::vector<TextureAsset> m_textures;
    std::vector<FontAsset> m_fonts;
    std::unordered_map<std::string, ModelHandle> m_modelCache;
    std::unordered_map<std::string, FontHandle> m_fontCache;
    uint64_t m_revision{1};
};
