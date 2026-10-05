#pragma once

#include "AssetStreaming.hpp"
#include "ProductionAssetPipeline.hpp"
#include "engine/diagnostics/Logger.hpp"

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
#include <span>
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
    // Linear-light RGBA texels of a high-dynamic-range source such as a Radiance
    // .hdr environment map, as IEEE half-float bits. When present, rgba and
    // compressed are empty and the format stays Rgba8.
    std::vector<uint16_t> rgbaHalf;
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

// KHR_materials_clearcoat: a clear dielectric coat (index of refraction 1.5)
// layered over the base material. A zero factor disables the layer.
struct MaterialClearcoat {
    float factor{0.0f};
    // Perceptual roughness of the coat.
    float roughness{0.0f};
    float normalScale{1.0f};
    // Red channel scales the factor.
    TextureHandle texture{};
    // Green channel scales the roughness.
    TextureHandle roughnessTexture{};
    // Tangent-space normal of the coat; without one the coat follows the
    // geometric normal rather than the base normal map.
    TextureHandle normalTexture{};
};

// KHR_materials_sheen: a retro-reflective fabric lobe (Charlie distribution)
// over the base. A black color disables the lobe.
struct MaterialSheen {
    glm::vec3 color{0.0f};
    float roughness{0.0f};
    // Color in rgb (sRGB encoded).
    TextureHandle colorTexture{};
    // Alpha channel scales the roughness.
    TextureHandle roughnessTexture{};
};

// KHR_materials_specular: strength and tint of the dielectric reflection.
// The defaults leave the metallic-roughness model unchanged.
struct MaterialSpecular {
    float factor{1.0f};
    // Linear tint of the dielectric reflectance at normal incidence.
    glm::vec3 color{1.0f};
    // Alpha channel scales the factor.
    TextureHandle texture{};
    // Color in rgb (sRGB encoded).
    TextureHandle colorTexture{};
};

// KHR_materials_transmission with the optional KHR_materials_volume. A zero
// factor disables transmission; a zero thickness keeps the surface thin-walled.
struct MaterialTransmission {
    // Fraction of the diffuse base replaced by light passing through.
    float factor{0.0f};
    // Red channel scales the factor.
    TextureHandle texture{};
    // Volume thickness in world units (the asset's mesh units, unscaled).
    float thickness{0.0f};
    // Linear color white light reaches after attenuationDistance in the medium.
    glm::vec3 attenuationColor{1.0f};
    // World units; infinity means the medium does not absorb.
    float attenuationDistance{std::numeric_limits<float>::infinity()};
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
    // KHR_materials_ior: index of refraction of dielectric parts; 1.5 gives the
    // metallic-roughness model's 4% reflectance.
    float ior{1.5f};
    MaterialClearcoat clearcoat{};
    MaterialSheen sheen{};
    MaterialSpecular specular{};
    MaterialTransmission transmission{};

    // True when the surface reveals what is behind it, so renderers draw it
    // after opaque geometry: alpha blending or transmission.
    bool translucent() const {
        return alphaMode == MaterialAlphaMode::Blend || transmission.factor > 0.0f;
    }

    // Every texture slot, absent ones included as empty handles; leases and
    // renderer uploads cover exactly this set.
    std::array<TextureHandle, 13> textures() const {
        return {baseColorTexture,    normalTexture,          metallicRoughnessTexture,   occlusionTexture,
                emissiveTexture,     clearcoat.texture,      clearcoat.roughnessTexture, clearcoat.normalTexture,
                sheen.colorTexture,  sheen.roughnessTexture, specular.texture,           specular.colorTexture,
                transmission.texture};
    }
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

// An object-space axis-aligned box. Empty (minimum above maximum) when it
// encloses nothing.
struct BoundingBox {
    glm::vec3 minimum{std::numeric_limits<float>::max()};
    glm::vec3 maximum{std::numeric_limits<float>::lowest()};

    bool empty() const {
        return minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z;
    }
};

// The box around every vertex position; empty without vertices.
BoundingBox calculateBoundingBox(std::span<const Vertex> vertices);

// A sphere around every vertex position, centred on their box; zero radius
// at the origin without vertices.
BoundingSphere calculateBoundingSphere(std::span<const Vertex> vertices);

struct MeshAsset {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitive> primitives;
    std::vector<MorphTarget> morphTargets;
    BoundingSphere bounds;
    // Box around every vertex, so tools need not scan the vertices each frame.
    // AssetManager computes it when the mesh is added; it is not serialized.
    BoundingBox localBox;
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
    // Materials referenced directly rather than through a mesh, such as a
    // MaterialOverrideComponent's replacement.
    std::vector<AssetLease<MaterialHandle>> materials;

    bool empty() const {
        return models.empty() && meshes.empty() && textures.empty() && fonts.empty() && materials.empty();
    }

    void clear() {
        models.clear();
        meshes.clear();
        textures.clear();
        fonts.clear();
        materials.clear();
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

// The material file (.zmat) a material was loaded from.
struct MaterialAssetSource {
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
    AssetManager() = default;
    // logger receives import warnings, possibly from asset worker threads,
    // and must outlive the manager; null writes them to stderr.
    explicit AssetManager(Logger* logger);

    AssetLoadResult<ModelHandle> loadModel(const std::filesystem::path& path);
    // Imports a fresh immutable model graph and updates future lookups while
    // retaining old handles for scenes and GPU frames that still use them.
    AssetLoadResult<ModelHandle> reloadModel(const std::filesystem::path& path);
    AssetLoadResult<MeshHandle> loadMesh(const std::filesystem::path& modelPath, uint32_t meshIndex);
    AssetLoadResult<FontHandle> loadFont(const std::filesystem::path& path);
    AssetLoadResult<TextureHandle> loadTexture(const std::filesystem::path& path);
    // Loads a material file (.zmat, see MaterialAssetFile.hpp) and the images
    // its texture slots name, relative to the file's folder; base color and
    // emissive images load as color, normal and metallic-roughness and
    // occlusion as linear RGBA8 data with their TextureUsage. A slot whose
    // image fails to load is left empty with a warning. Loads are cached by
    // path, like models.
    AssetLoadResult<MaterialHandle> loadMaterial(const std::filesystem::path& path);
    // Reads the file again into a fresh material and updates future lookups.
    // Materials are immutable, so the previous handle stays valid for those
    // still using it: point them at the new handle (MaterialOverrideComponent
    // users), and collectUnused frees the old one once nothing leases it.
    AssetLoadResult<MaterialHandle> reloadMaterial(const std::filesystem::path& path);

    // Runtime content: assets that code builds instead of importing, such as
    // procedural geometry or tinted materials (MeshBuilder makes meshes).
    // Each is validated (a failure returns a descriptive error and adds
    // nothing) and gets a fresh generational handle with no source path, so
    // no load call finds it and a scene that references one cannot be saved.
    // Like imported assets they are leased and collected: collectUnused
    // evicts one that nothing leases, so a creator leases what it keeps until
    // a scene that holds it is leased. Runtime assets are immutable; to change
    // one, create its replacement, point the users at the new handle, and let
    // the old one be collected. debugName labels it in diagnostics.
    //
    // createMesh recomputes bounds and localBox from the vertices. Every
    // primitive needs a material of this manager and whole triangles inside
    // the index buffer (simplified LOD ranges too), and every index must
    // address a vertex.
    AssetLoadResult<MeshHandle> createMesh(MeshAsset asset, std::string debugName);
    // Every texture slot must be empty or name a live texture of this manager.
    AssetLoadResult<MaterialHandle> createMaterial(MaterialAsset asset, std::string debugName);
    // RGBA8 needs width * height * 4 bytes in rgba (or half floats in
    // rgbaHalf, for environment maps); block-compressed formats need a
    // complete, valid mip chain.
    AssetLoadResult<TextureHandle> createTexture(TextureAsset asset, std::string debugName);

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
    // The file a loadMaterial material came from; nullopt for model and
    // runtime materials.
    std::optional<MaterialAssetSource> materialSource(MaterialHandle handle) const;
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

    const LogChannel& log() const {
        return m_log;
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
    AssetLoadResult<MaterialHandle> importMaterial(const std::filesystem::path& path, const std::string& cacheKey);
    // A standalone RGBA8 texture read with usage; color textures share
    // loadTexture's cache entry.
    AssetLoadResult<TextureHandle> loadTextureWithUsage(const std::filesystem::path& path, TextureUsage usage);

    struct SlotMetadata {
        uint32_t generation{1};
        bool occupied{true};
        uint64_t cpuBytes{0};
        uint64_t gpuBytes{0};
        uint64_t lastUse{0};
        std::shared_ptr<std::atomic_uint32_t> references{std::make_shared<std::atomic_uint32_t>(0)};
        // Set for runtime content (createMesh and friends); shown in diagnostics.
        std::string debugName;
    };

    std::vector<ModelAsset> m_models;
    std::vector<ModelAssetSource> m_modelSources;
    std::vector<MeshAsset> m_meshes;
    std::vector<MeshAssetSource> m_meshSources;
    std::vector<MaterialAsset> m_materials;
    std::vector<MaterialAssetSource> m_materialSources;
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
    std::unordered_map<std::string, MaterialHandle> m_materialCache;
    uint64_t m_revision{1};
    uint64_t m_useSerial{1};
    AssetMemoryBudget m_budget;
    DerivedAssetResidencyManager m_streaming;
    std::filesystem::path m_cookedTextureRoot;
    TextureFormatCapabilities m_textureCapabilities;
    LogChannel m_log;
};
