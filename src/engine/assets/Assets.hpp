#pragma once

#include "AssetStreaming.hpp"
#include "ProductionAssetPipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <memory>
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

enum class AssetKind : uint8_t {
    Model,
    Mesh,
    Material,
    Texture,
    Font,
};

struct AssetMemoryBudget {
    uint64_t cpuBytes{std::numeric_limits<uint64_t>::max()};
};

struct AssetCollectionResult {
    uint32_t assetsEvicted{0};
    uint64_t bytesFreed{0};
    uint64_t bytesRetained{0};
    bool budgetSatisfied{true};
};

struct AssetDiagnostic {
    AssetKind kind{AssetKind::Model};
    uint32_t index{0};
    uint32_t generation{0};
    uint32_t strongReferences{0};
    uint64_t cpuBytes{0};
    uint64_t gpuBytes{0};
    std::string source;
};

template <typename Handle> class AssetLease {
  public:
    AssetLease() = default;

    // Constructed by AssetManager; public to keep the small type-erased lease
    // token independent of individual asset slot implementations.
    AssetLease(Handle handle, std::shared_ptr<void> token)
        : m_handle(handle)
        , m_token(std::move(token)) {}

    Handle handle() const {
        return m_handle;
    }

    explicit operator bool() const {
        return static_cast<bool>(m_handle) && static_cast<bool>(m_token);
    }

  private:
    friend class AssetManager;
    Handle m_handle{};
    std::shared_ptr<void> m_token;
};

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 texCoord{};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::uvec4 joints{0};
    glm::vec4 weights{0.0f};
};

// Pixel storage of a TextureAsset. Block-compressed formats use 4x4 texel
// blocks of 16 bytes; color space is chosen per material slot at sampling time.
enum class TextureFormat : uint8_t {
    Rgba8,
    Bc7,
    Bc5,
};

// What a texture's channels mean, derived from the material slots that
// reference it. Color is sRGB-encoded; Normal stores a tangent-space XY pair
// (Z is reconstructed in shaders); Data holds linear values such as ORM.
enum class TextureUsage : uint8_t {
    Color,
    Normal,
    Data,
};

struct TextureMipLevel {
    uint32_t width{0};
    uint32_t height{0};
    // Byte range of the level inside TextureAsset::compressed.
    uint64_t offset{0};
    uint64_t size{0};
};

struct TextureAsset {
    uint32_t width{0};
    uint32_t height{0};
    // RGBA8 base level, present only when format is Rgba8. The renderer
    // generates the remaining mip levels for this format.
    std::vector<uint8_t> rgba;
    TextureFormat format{TextureFormat::Rgba8};
    TextureUsage usage{TextureUsage::Color};
    // Complete block-compressed mip chain (level 0 first, packed back to back),
    // present only for block-compressed formats.
    std::vector<uint8_t> compressed;
    std::vector<TextureMipLevel> mips;

    bool blockCompressed() const {
        return format != TextureFormat::Rgba8;
    }
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

// One level of detail of a primitive: an index range into MeshAsset::indices
// that addresses the same vertices as the full-detail range.
struct MeshLod {
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    // Object-space distance the simplified surface may deviate from the full-
    // detail surface; zero for the full-detail level.
    float geometricError{0.0f};
};

// Level 0 plus up to three simplified levels.
inline constexpr uint32_t maximumMeshLods = 4;

struct MeshPrimitive {
    // Full-detail (LOD 0) index range.
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    MaterialHandle material{};
    // Progressively coarser ranges (LOD 1, 2, ...) with strictly decreasing
    // index counts and non-decreasing geometric errors.
    std::array<MeshLod, maximumMeshLods - 1> simplifiedLods{};
    uint32_t simplifiedLodCount{0};

    uint32_t lodCount() const {
        return 1 + simplifiedLodCount;
    }

    // Level 0 is the full-detail range; levels past the coarsest clamp to it.
    MeshLod lod(uint32_t level) const {
        if (level == 0 || simplifiedLodCount == 0) {
            return {.firstIndex = firstIndex, .indexCount = indexCount, .geometricError = 0.0f};
        }
        return simplifiedLods[std::min(level, simplifiedLodCount) - 1];
    }
};

struct MorphTarget {
    std::string name;
    std::vector<glm::vec3> positionDeltas;
    std::vector<glm::vec3> normalDeltas;
    std::vector<glm::vec3> tangentDeltas;
};

struct BoundingSphere {
    glm::vec3 center{0.0f};
    float radius{0.0f};
};

struct MeshAsset {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitive> primitives;
    std::vector<MorphTarget> morphTargets;
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
    std::vector<float> morphWeights;
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
    Weights,
};

struct AnimationChannel {
    uint32_t targetNode{0};
    AnimationPath path{AnimationPath::Translation};
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    std::vector<float> times;
    std::vector<glm::vec4> values;
    std::vector<glm::vec4> inTangents;
    std::vector<glm::vec4> outTangents;
    std::vector<std::vector<float>> weightValues;
    std::vector<std::vector<float>> weightInTangents;
    std::vector<std::vector<float>> weightOutTangents;
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

struct AssetLeaseSet {
    std::vector<AssetLease<ModelHandle>> models;
    std::vector<AssetLease<MeshHandle>> meshes;
    std::vector<AssetLease<TextureHandle>> textures;
    std::vector<AssetLease<FontHandle>> fonts;

    bool empty() const {
        return models.empty() && meshes.empty() && textures.empty() && fonts.empty();
    }

    void clear() {
        models.clear();
        meshes.clear();
        textures.clear();
        fonts.clear();
    }
};

class Scene;

struct MeshAssetSource {
    std::filesystem::path path;
    uint32_t meshIndex{0};
};

struct TextureAssetSource {
    std::filesystem::path path;
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
    AssetLoadResult<TextureHandle> loadTexture(const std::filesystem::path& path);
    const ModelAsset* model(ModelHandle handle) const;
    const MeshAsset* mesh(MeshHandle handle) const;
    const MaterialAsset* material(MaterialHandle handle) const;
    const TextureAsset* texture(TextureHandle handle) const;
    const FontAsset* font(FontHandle handle) const;
    size_t fontCount() const;
    std::vector<FontHandle> fontHandles() const;
    std::optional<ModelAssetSource> modelSource(ModelHandle handle) const;
    std::optional<MeshAssetSource> meshSource(MeshHandle handle) const;
    std::optional<TextureAssetSource> textureSource(TextureHandle handle) const;
    uint64_t revision() const;
    AssetLease<ModelHandle> lease(ModelHandle handle) const;
    AssetLease<MeshHandle> lease(MeshHandle handle) const;
    AssetLease<MaterialHandle> lease(MaterialHandle handle) const;
    AssetLease<TextureHandle> lease(TextureHandle handle) const;
    AssetLease<FontHandle> lease(FontHandle handle) const;
    AssetLeaseSet leaseSceneAssets(const Scene& scene) const;
    void setMemoryBudget(AssetMemoryBudget budget);
    AssetMemoryBudget memoryBudget() const;
    AssetCollectionResult collectUnused(bool force = false);
    std::vector<AssetDiagnostic> diagnostics() const;
    uint64_t cpuBytes() const;

    // Model imports replace external source images with cooked
    // block-compressed textures from this derived-data root when the renderer
    // can sample the cooked format (see CompressedTexture.hpp). An empty root
    // or unsupported format keeps the decoded RGBA8 source image.
    void setCookedTextureRoot(std::filesystem::path root);
    void setTextureFormatCapabilities(TextureFormatCapabilities capabilities);

    const std::filesystem::path& cookedTextureRoot() const {
        return m_cookedTextureRoot;
    }

    const TextureFormatCapabilities& textureFormatCapabilities() const {
        return m_textureCapabilities;
    }

    DerivedAssetResidencyManager& streaming() {
        return m_streaming;
    }

    const DerivedAssetResidencyManager& streaming() const {
        return m_streaming;
    }

  private:
    friend class ModelImporter;
    ModelHandle addModel(ModelAsset asset, std::filesystem::path sourcePath);
    MeshHandle addMesh(MeshAsset asset, std::filesystem::path sourcePath, uint32_t meshIndex);
    MaterialHandle addMaterial(MaterialAsset asset);
    TextureHandle addTexture(TextureAsset asset);
    AssetLoadResult<ModelHandle> importModel(const std::filesystem::path& path, const std::string& cacheKey);

    struct SlotMetadata {
        uint32_t generation{1};
        bool occupied{true};
        uint64_t cpuBytes{0};
        uint64_t gpuBytes{0};
        uint64_t lastUse{0};
        std::shared_ptr<std::atomic_uint32_t> references{std::make_shared<std::atomic_uint32_t>(0)};
    };

    std::vector<ModelAsset> m_models;
    std::vector<ModelAssetSource> m_modelSources;
    std::vector<MeshAsset> m_meshes;
    std::vector<MeshAssetSource> m_meshSources;
    std::vector<MaterialAsset> m_materials;
    std::vector<TextureAsset> m_textures;
    std::vector<TextureAssetSource> m_textureSources;
    std::vector<FontAsset> m_fonts;
    std::vector<SlotMetadata> m_modelMetadata;
    std::vector<SlotMetadata> m_meshMetadata;
    std::vector<SlotMetadata> m_materialMetadata;
    std::vector<SlotMetadata> m_textureMetadata;
    std::vector<SlotMetadata> m_fontMetadata;
    std::unordered_map<std::string, ModelHandle> m_modelCache;
    std::unordered_map<std::string, FontHandle> m_fontCache;
    std::unordered_map<std::string, TextureHandle> m_textureCache;
    uint64_t m_revision{1};
    uint64_t m_useSerial{1};
    AssetMemoryBudget m_budget;
    DerivedAssetResidencyManager m_streaming;
    std::filesystem::path m_cookedTextureRoot;
    TextureFormatCapabilities m_textureCapabilities;
};
