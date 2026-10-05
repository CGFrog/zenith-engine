#include "Assets.hpp"
#include "CompressedTexture.hpp"
#include "ImageDecoder.hpp"
#include "MaterialAssetFile.hpp"
#include "ModelImporter.hpp"
#include "PackedVertexStreams.hpp"
#include "engine/platform/FileContents.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>

namespace {
template <typename T, typename Handle, typename Metadata>
const T* resolve(const std::vector<T>& assets, const std::vector<Metadata>& metadata, Handle handle) {
    return handle && handle.index < assets.size() && handle.index < metadata.size() &&
                   metadata[handle.index].occupied && metadata[handle.index].generation == handle.generation
               ? &assets[handle.index]
               : nullptr;
}

uint64_t byteSize(const TextureAsset& asset) {
    return sizeof(TextureAsset) + asset.rgba.size() + asset.rgbaHalf.size() * sizeof(uint16_t) +
           asset.compressed.size() + asset.mips.size() * sizeof(TextureMipLevel);
}

uint64_t gpuByteSize(const TextureAsset& asset) {
    if (asset.blockCompressed()) {
        // Cooked textures upload their stored mip chain unchanged.
        return asset.compressed.size();
    }
    if (!asset.rgbaHalf.empty()) {
        // High-dynamic-range environment sources live on the GPU only while
        // EnvironmentPass converts them into lighting cubes.
        return 0;
    }
    uint64_t bytes = 0;
    uint32_t width = asset.width;
    uint32_t height = asset.height;
    while (width > 0 && height > 0) {
        bytes += static_cast<uint64_t>(width) * height * 4;
        if (width == 1 && height == 1) {
            break;
        }
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
    }
    return bytes;
}

uint64_t byteSize(const FontAsset& asset) {
    return sizeof(FontAsset) + asset.bytes.size();
}

uint64_t byteSize(const MaterialAsset&) {
    return sizeof(MaterialAsset);
}

uint64_t byteSize(const MeshAsset& asset) {
    uint64_t bytes = sizeof(MeshAsset) + asset.vertices.size() * sizeof(Vertex) +
                     asset.indices.size() * sizeof(uint32_t) + asset.primitives.size() * sizeof(MeshPrimitive) +
                     asset.morphTargets.size() * sizeof(MorphTarget);
    for (const MorphTarget& target : asset.morphTargets) {
        bytes += target.name.size() +
                 (target.positionDeltas.size() + target.normalDeltas.size() + target.tangentDeltas.size()) *
                     sizeof(glm::vec3);
    }
    return bytes;
}

uint64_t gpuByteSize(const MeshAsset& asset) {
    // Vertices upload as quantized split streams; see PackedVertexStreams.hpp.
    return vertex_packing::packedByteSize(asset.vertices) + asset.indices.size() * sizeof(uint32_t);
}

uint64_t byteSize(const ModelAsset& asset) {
    uint64_t bytes = sizeof(ModelAsset) + asset.nodes.size() * sizeof(ModelNode) +
                     asset.skins.size() * sizeof(SkinAsset) + asset.animations.size() * sizeof(AnimationClip);
    for (const ModelNode& node : asset.nodes) {
        bytes += node.name.size() + node.morphWeights.size() * sizeof(float);
    }
    for (const SkinAsset& skin : asset.skins) {
        bytes += skin.name.size() + skin.joints.size() * sizeof(uint32_t) +
                 skin.inverseBindMatrices.size() * sizeof(glm::mat4);
    }
    for (const AnimationClip& clip : asset.animations) {
        bytes += clip.name.size();
        for (const AnimationChannel& channel : clip.channels) {
            bytes += channel.times.size() * sizeof(float) + channel.values.size() * sizeof(glm::vec4) +
                     channel.inTangents.size() * sizeof(glm::vec4) + channel.outTangents.size() * sizeof(glm::vec4);
            const auto nestedFloats = [](const std::vector<std::vector<float>>& values) {
                return std::accumulate(
                    values.begin(), values.end(), uint64_t{0},
                    [](uint64_t total, const auto& value) { return total + value.size() * sizeof(float); });
            };
            bytes += nestedFloats(channel.weightValues) + nestedFloats(channel.weightInTangents) +
                     nestedFloats(channel.weightOutTangents);
        }
    }
    return bytes;
}

template <typename T, typename Handle, typename Metadata>
Handle insertSlot(std::vector<T>& assets, std::vector<Metadata>& metadata, T asset, uint64_t bytes,
                  uint64_t useSerial) {
    for (uint32_t index = 0; index < metadata.size(); ++index) {
        if (!metadata[index].occupied) {
            assets[index] = std::move(asset);
            metadata[index].occupied = true;
            metadata[index].cpuBytes = bytes;
            metadata[index].lastUse = useSerial;
            metadata[index].references = std::make_shared<std::atomic_uint32_t>(0);
            metadata[index].debugName.clear();
            return {index, metadata[index].generation};
        }
    }
    assets.push_back(std::move(asset));
    Metadata entry;
    entry.cpuBytes = bytes;
    entry.lastUse = useSerial;
    metadata.push_back(std::move(entry));
    return {static_cast<uint32_t>(assets.size() - 1), metadata.back().generation};
}

struct LeaseToken {
    explicit LeaseToken(std::shared_ptr<std::atomic_uint32_t> value)
        : references(std::move(value)) {
        references->fetch_add(1, std::memory_order_relaxed);
    }

    ~LeaseToken() {
        references->fetch_sub(1, std::memory_order_relaxed);
    }

    std::shared_ptr<std::atomic_uint32_t> references;
};

struct CompositeLeaseToken {
    std::vector<std::shared_ptr<void>> tokens;
};

std::pair<std::filesystem::path, std::string> normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path normalized = error ? path.lexically_normal() : canonical;
    return {normalized, normalized.generic_string()};
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const glm::vec4& value) {
    return finite(glm::vec3(value)) && std::isfinite(value.w);
}

// Why an index range cannot be drawn, or empty when it can.
std::string indexRangeError(uint32_t firstIndex, uint32_t indexCount, size_t indexTotal) {
    if (indexCount == 0 || indexCount % 3 != 0) {
        return "index count " + std::to_string(indexCount) + " is not a positive multiple of three";
    }
    if (static_cast<uint64_t>(firstIndex) + indexCount > indexTotal) {
        return "indices " + std::to_string(firstIndex) + ".." + std::to_string(uint64_t{firstIndex} + indexCount) +
               " reach past the " + std::to_string(indexTotal) + " indices";
    }
    return {};
}

std::string primitiveError(const MeshPrimitive& primitive, size_t indexTotal) {
    if (std::string error = indexRangeError(primitive.firstIndex, primitive.indexCount, indexTotal); !error.empty()) {
        return error;
    }
    if (primitive.simplifiedLodCount > primitive.simplifiedLods.size()) {
        return "it claims " + std::to_string(primitive.simplifiedLodCount) + " simplified levels";
    }
    uint32_t previousCount = primitive.indexCount;
    for (uint32_t level = 0; level < primitive.simplifiedLodCount; ++level) {
        const MeshLod& lod = primitive.simplifiedLods[level];
        if (std::string error = indexRangeError(lod.firstIndex, lod.indexCount, indexTotal); !error.empty()) {
            return "LOD " + std::to_string(level + 1) + ": " + error;
        }
        if (lod.indexCount >= previousCount || !std::isfinite(lod.geometricError) || lod.geometricError < 0.0f) {
            return "LOD " + std::to_string(level + 1) +
                   " does not have fewer indices than the level before it and a finite geometric error";
        }
        previousCount = lod.indexCount;
    }
    return {};
}

std::string runtimeMeshError(const MeshAsset& mesh, const AssetManager& assets) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.primitives.empty()) {
        return "it needs vertices, indices, and at least one primitive";
    }
    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max()) {
        return "it has more vertices than 32-bit indices address";
    }
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        const Vertex& vertex = mesh.vertices[index];
        if (!finite(vertex.position) || !finite(vertex.normal) || !finite(vertex.tangent) ||
            !std::isfinite(vertex.texCoord.x) || !std::isfinite(vertex.texCoord.y) || !finite(vertex.weights)) {
            return "vertex " + std::to_string(index) + " is not finite";
        }
    }
    const auto outOfRange =
        std::ranges::find_if(mesh.indices, [&](uint32_t index) { return index >= mesh.vertices.size(); });
    if (outOfRange != mesh.indices.end()) {
        return "index " + std::to_string(*outOfRange) + " at position " +
               std::to_string(std::distance(mesh.indices.begin(), outOfRange)) + " addresses no vertex (there are " +
               std::to_string(mesh.vertices.size()) + ")";
    }
    for (size_t index = 0; index < mesh.primitives.size(); ++index) {
        const MeshPrimitive& primitive = mesh.primitives[index];
        if (std::string error = primitiveError(primitive, mesh.indices.size()); !error.empty()) {
            return "primitive " + std::to_string(index) + ": " + error;
        }
        if (assets.material(primitive.material) == nullptr) {
            return "primitive " + std::to_string(index) + " has no live material of this asset manager";
        }
    }
    for (const MorphTarget& target : mesh.morphTargets) {
        const auto sized = [&](const std::vector<glm::vec3>& deltas) {
            return deltas.empty() || deltas.size() == mesh.vertices.size();
        };
        if (!sized(target.positionDeltas) || !sized(target.normalDeltas) || !sized(target.tangentDeltas)) {
            return "morph target '" + target.name + "' does not have one delta per vertex";
        }
    }
    return {};
}

std::string runtimeMaterialError(const MaterialAsset& material, const AssetManager& assets) {
    const bool finiteFactors =
        finite(material.baseColor) && finite(material.emissive) && std::isfinite(material.emissiveStrength) &&
        std::isfinite(material.metallic) && std::isfinite(material.roughness) && std::isfinite(material.normalScale) &&
        std::isfinite(material.occlusionStrength) && std::isfinite(material.alphaCutoff) && std::isfinite(material.ior);
    if (!finiteFactors) {
        return "its factors are not finite";
    }
    const std::array<TextureHandle, 13> textures = material.textures();
    for (size_t slot = 0; slot < textures.size(); ++slot) {
        if (textures[slot] && assets.texture(textures[slot]) == nullptr) {
            return "texture slot " + std::to_string(slot) + " names no live texture of this asset manager";
        }
    }
    return {};
}

std::string runtimeTextureError(const TextureAsset& texture) {
    constexpr uint32_t maximumExtent = 16384;
    if (texture.width == 0 || texture.height == 0 || texture.width > maximumExtent || texture.height > maximumExtent) {
        return "its size " + std::to_string(texture.width) + "x" + std::to_string(texture.height) + " is outside 1.." +
               std::to_string(maximumExtent);
    }
    if (texture.blockCompressed()) {
        const AssetPipelineResult valid = validateBlockCompressedTexture(texture);
        return valid ? std::string{} : valid.error;
    }
    const size_t texels = static_cast<size_t>(texture.width) * texture.height * 4;
    const bool eightBit = texture.rgba.size() == texels && texture.rgbaHalf.empty();
    const bool halfFloat = texture.rgbaHalf.size() == texels && texture.rgba.empty();
    if (!eightBit && !halfFloat) {
        return "RGBA8 data needs exactly width * height * 4 values in rgba (or rgbaHalf), not " +
               std::to_string(texture.rgba.size()) + " and " + std::to_string(texture.rgbaHalf.size());
    }
    if (!texture.compressed.empty() || !texture.mips.empty()) {
        return "an RGBA8 texture carries no compressed mip chain";
    }
    return {};
}
} // namespace

BoundingBox calculateBoundingBox(std::span<const Vertex> vertices) {
    BoundingBox box;
    for (const Vertex& vertex : vertices) {
        box.minimum = glm::min(box.minimum, vertex.position);
        box.maximum = glm::max(box.maximum, vertex.position);
    }
    return box;
}

BoundingSphere calculateBoundingSphere(std::span<const Vertex> vertices) {
    const BoundingBox box = calculateBoundingBox(vertices);
    if (box.empty()) {
        return {};
    }
    const glm::vec3 center = (box.minimum + box.maximum) * 0.5f;
    float radiusSquared = 0.0f;
    for (const Vertex& vertex : vertices) {
        const glm::vec3 offset = vertex.position - center;
        radiusSquared = std::max(radiusSquared, glm::dot(offset, offset));
    }
    return {.center = center, .radius = std::sqrt(radiusSquared)};
}

AssetManager::AssetManager(Logger* logger)
    : m_log(logger, "assets") {}

AssetLoadResult<ModelHandle> AssetManager::loadModel(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    if (const auto found = m_modelCache.find(key); found != m_modelCache.end()) {
        return {.handle = found->second};
    }

    return importModel(normalized, key);
}

AssetLoadResult<ModelHandle> AssetManager::reloadModel(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    return importModel(normalized, key);
}

AssetLoadResult<ModelHandle> AssetManager::importModel(const std::filesystem::path& path, const std::string& cacheKey) {
    // Importers build an immutable asset graph incrementally. Preserve the
    // complete slot state because an import may reuse an evicted slot before a
    // later primitive, skin, or animation validation error is discovered.
    const auto models = m_models;
    const auto modelSources = m_modelSources;
    const auto meshes = m_meshes;
    const auto meshSources = m_meshSources;
    const auto materials = m_materials;
    const auto materialSources = m_materialSources;
    const auto textures = m_textures;
    const auto textureSources = m_textureSources;
    const auto modelMetadata = m_modelMetadata;
    const auto meshMetadata = m_meshMetadata;
    const auto materialMetadata = m_materialMetadata;
    const auto textureMetadata = m_textureMetadata;
    const uint64_t revision = m_revision;
    const uint64_t useSerial = m_useSerial;

    ModelImporter importer(*this);
    auto result = importer.load(path);

    if (result) {
        m_modelCache.insert_or_assign(cacheKey, result.handle);
    } else {
        m_models = models;
        m_modelSources = modelSources;
        m_meshes = meshes;
        m_meshSources = meshSources;
        m_materials = materials;
        m_materialSources = materialSources;
        m_textures = textures;
        m_textureSources = textureSources;
        m_modelMetadata = modelMetadata;
        m_meshMetadata = meshMetadata;
        m_materialMetadata = materialMetadata;
        m_textureMetadata = textureMetadata;
        m_revision = revision;
        m_useSerial = useSerial;
    }

    return result;
}

AssetLoadResult<MeshHandle> AssetManager::loadMesh(const std::filesystem::path& modelPath, uint32_t meshIndex) {
    const AssetLoadResult<ModelHandle> modelResult = loadModel(modelPath);
    if (!modelResult) {
        return {.error = modelResult.error};
    }

    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(modelPath, error);
    const std::filesystem::path normalized = error ? modelPath.lexically_normal() : canonical;
    for (size_t index = 0; index < m_meshSources.size(); ++index) {
        if (m_meshSources[index].path == normalized && m_meshSources[index].meshIndex == meshIndex) {
            if (index < m_meshMetadata.size() && m_meshMetadata[index].occupied) {
                return {.handle = MeshHandle{static_cast<uint32_t>(index), m_meshMetadata[index].generation}};
            }
        }
    }
    return {.error = "Model does not contain mesh index " + std::to_string(meshIndex) + ": " + modelPath.string()};
}

AssetLoadResult<FontHandle> AssetManager::loadFont(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    if (const auto found = m_fontCache.find(key); found != m_fontCache.end()) {
        return {.handle = found->second};
    }

    FontAsset asset;
    if (readFileBytes(normalized, asset.bytes) == FileReadStatus::OpenFailed) {
        return {.error = "Could not open font: " + normalized.string()};
    }
    if (asset.bytes.empty()) {
        return {.error = "Font file is empty: " + normalized.string()};
    }
    const uint64_t bytes = byteSize(asset);
    const FontHandle handle =
        insertSlot<FontAsset, FontHandle>(m_fonts, m_fontMetadata, std::move(asset), bytes, ++m_useSerial);
    m_fontCache.emplace(key, handle);
    return {.handle = handle};
}

AssetLoadResult<TextureHandle> AssetManager::loadTexture(const std::filesystem::path& path) {
    return loadTexture(path, TextureUsage::Color);
}

namespace {
// The cache key suffix of a standalone texture read with usage.
std::string textureUsageKeySuffix(TextureUsage usage) {
    switch (usage) {
    case TextureUsage::Color:
        return {};
    case TextureUsage::Normal:
        return "#normal";
    case TextureUsage::Data:
        return "#data";
    }
    return {};
}

// Decodes a standalone texture, which must be RGBA8.
ImageDecodeResult decodeStandaloneTexture(const std::filesystem::path& path) {
    ImageDecodeResult decoded = decodeImageFile(path);
    if (decoded && decoded.texture.blockCompressed()) {
        // Standalone textures feed CPU consumers such as environment
        // prefiltering, which read RGBA8 texels.
        ImageDecodeResult refused;
        refused.error = "Standalone textures must be RGBA8; block-compressed KTX2 is only supported for model "
                        "materials: " +
                        path.string();
        return refused;
    }
    return decoded;
}
} // namespace

AssetLoadResult<TextureHandle> AssetManager::loadTexture(const std::filesystem::path& path, TextureUsage usage) {
    auto [normalized, key] = normalizedPath(path);
    // One file read as color and as data is two textures.
    key += textureUsageKeySuffix(usage);
    if (const auto found = m_textureCache.find(key); found != m_textureCache.end()) {
        return {.handle = found->second};
    }
    ImageDecodeResult decoded = decodeStandaloneTexture(normalized);
    if (!decoded) {
        return {.error = decoded.error};
    }
    decoded.texture.usage = usage;
    return {.handle = addSourceTexture(std::move(decoded.texture), normalized, key)};
}

TextureHandle AssetManager::addSourceTexture(TextureAsset texture, const std::filesystem::path& path,
                                             const std::string& cacheKey) {
    const TextureHandle handle = addTexture(std::move(texture));
    if (handle.index >= m_textureSources.size()) {
        m_textureSources.resize(handle.index + 1);
    }
    m_textureSources[handle.index] = {path};
    m_textureCache.insert_or_assign(cacheKey, handle);
    return handle;
}

TextureReloadResult AssetManager::reloadTexture(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    std::vector<std::pair<TextureUsage, TextureHandle>> loaded;
    for (const TextureUsage usage : {TextureUsage::Color, TextureUsage::Normal, TextureUsage::Data}) {
        if (const auto found = m_textureCache.find(key + textureUsageKeySuffix(usage)); found != m_textureCache.end()) {
            loaded.emplace_back(usage, found->second);
        }
    }
    TextureReloadResult result{.success = true};
    if (loaded.empty()) {
        return result;
    }
    const ImageDecodeResult decoded = decodeStandaloneTexture(normalized);
    if (!decoded) {
        return {.error = decoded.error};
    }
    for (const auto& [usage, previous] : loaded) {
        TextureAsset texture = decoded.texture;
        texture.usage = usage;
        const TextureHandle current =
            addSourceTexture(std::move(texture), normalized, key + textureUsageKeySuffix(usage));
        result.replaced.push_back({.previous = previous, .current = current});
    }
    // Material files load their images through this cache, so reloading one
    // picks up the replacement.
    for (const auto& [materialKey, handle] : m_materialCache) {
        const MaterialAsset* asset = material(handle);
        if (asset == nullptr || handle.index >= m_materialSources.size()) {
            continue;
        }
        const std::array<TextureHandle, 13> used = asset->textures();
        const bool samplesReplaced = std::ranges::any_of(result.replaced, [&used](const TextureReplacement& entry) {
            return std::ranges::find(used, entry.previous) != used.end();
        });
        if (samplesReplaced) {
            result.materialFiles.push_back(m_materialSources[handle.index].path);
        }
    }
    std::ranges::sort(result.materialFiles);
    return result;
}

AssetLoadResult<MaterialHandle> AssetManager::loadMaterial(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    if (const auto found = m_materialCache.find(key); found != m_materialCache.end()) {
        return {.handle = found->second};
    }
    return importMaterial(normalized, key);
}

AssetLoadResult<MaterialHandle> AssetManager::reloadMaterial(const std::filesystem::path& path) {
    const auto [normalized, key] = normalizedPath(path);
    return importMaterial(normalized, key);
}

AssetLoadResult<MaterialHandle> AssetManager::importMaterial(const std::filesystem::path& path,
                                                             const std::string& cacheKey) {
    MaterialFile file;
    if (const MaterialFileResult read = readMaterialFile(path, file); !read) {
        return {.error = read.error + ": " + path.string()};
    }
    MaterialAsset material{.baseColor = file.baseColor,
                           .emissive = file.emissive,
                           .emissiveStrength = file.emissiveStrength,
                           .metallic = file.metallic,
                           .roughness = file.roughness,
                           .normalScale = file.normalScale,
                           .occlusionStrength = file.occlusionStrength,
                           .alphaMode = file.alphaMode,
                           .alphaCutoff = file.alphaCutoff,
                           .doubleSided = file.doubleSided};
    std::array<TextureHandle*, materialTextureSlotCount> slots{&material.baseColorTexture, &material.normalTexture,
                                                               &material.metallicRoughnessTexture,
                                                               &material.occlusionTexture, &material.emissiveTexture};
    for (size_t slot = 0; slot < materialTextureSlotCount; ++slot) {
        const MaterialTextureReference& reference = file.textures[slot];
        if (reference.path.empty()) {
            continue;
        }
        const std::filesystem::path image =
            path.parent_path() / std::filesystem::path(std::u8string(reference.path.begin(), reference.path.end()));
        const AssetLoadResult<TextureHandle> loaded =
            loadTexture(image, materialTextureUsage(static_cast<MaterialTextureSlot>(slot)));
        if (!loaded) {
            m_log.warning("Material " + path.string() + " " +
                          materialTextureKey(static_cast<MaterialTextureSlot>(slot)) + ": " + loaded.error);
            continue;
        }
        *slots[slot] = loaded.handle;
    }
    const MaterialHandle handle = addMaterial(std::move(material));
    m_materialSources[handle.index] = {path};
    m_materialCache.insert_or_assign(cacheKey, handle);
    return {.handle = handle};
}

AssetLoadResult<MeshHandle> AssetManager::createMesh(MeshAsset asset, std::string debugName) {
    if (const std::string error = runtimeMeshError(asset, *this); !error.empty()) {
        return {.error = "Cannot create mesh '" + debugName + "': " + error};
    }
    asset.bounds = calculateBoundingSphere(asset.vertices);
    const MeshHandle handle = addMesh(std::move(asset), {}, 0);
    m_meshMetadata[handle.index].debugName = std::move(debugName);
    return {.handle = handle};
}

AssetLoadResult<MaterialHandle> AssetManager::createMaterial(MaterialAsset asset, std::string debugName) {
    if (const std::string error = runtimeMaterialError(asset, *this); !error.empty()) {
        return {.error = "Cannot create material '" + debugName + "': " + error};
    }
    const MaterialHandle handle = addMaterial(std::move(asset));
    m_materialMetadata[handle.index].debugName = std::move(debugName);
    return {.handle = handle};
}

AssetLoadResult<TextureHandle> AssetManager::createTexture(TextureAsset asset, std::string debugName) {
    if (const std::string error = runtimeTextureError(asset); !error.empty()) {
        return {.error = "Cannot create texture '" + debugName + "': " + error};
    }
    const TextureHandle handle = addTexture(std::move(asset));
    m_textureMetadata[handle.index].debugName = std::move(debugName);
    return {.handle = handle};
}

const ModelAsset* AssetManager::model(ModelHandle handle) const {
    return resolve(m_models, m_modelMetadata, handle);
}

const MeshAsset* AssetManager::mesh(MeshHandle handle) const {
    return resolve(m_meshes, m_meshMetadata, handle);
}

const MaterialAsset* AssetManager::material(MaterialHandle handle) const {
    return resolve(m_materials, m_materialMetadata, handle);
}

const TextureAsset* AssetManager::texture(TextureHandle handle) const {
    return resolve(m_textures, m_textureMetadata, handle);
}

const FontAsset* AssetManager::font(FontHandle handle) const {
    return resolve(m_fonts, m_fontMetadata, handle);
}

size_t AssetManager::fontCount() const {
    return static_cast<size_t>(
        std::ranges::count_if(m_fontMetadata, [](const SlotMetadata& slot) { return slot.occupied; }));
}

std::vector<FontHandle> AssetManager::fontHandles() const {
    std::vector<FontHandle> handles;
    handles.reserve(fontCount());
    for (uint32_t index = 0; index < m_fontMetadata.size(); ++index) {
        const SlotMetadata& slot = m_fontMetadata[index];
        if (slot.occupied) {
            handles.push_back({index, slot.generation});
        }
    }
    return handles;
}

std::optional<ModelAssetSource> AssetManager::modelSource(ModelHandle handle) const {
    return model(handle) != nullptr && handle.index < m_modelSources.size()
               ? std::optional<ModelAssetSource>{m_modelSources[handle.index]}
               : std::nullopt;
}

std::optional<MeshAssetSource> AssetManager::meshSource(MeshHandle handle) const {
    // Runtime meshes have no source path.
    return mesh(handle) != nullptr && handle.index < m_meshSources.size() && !m_meshSources[handle.index].path.empty()
               ? std::optional<MeshAssetSource>{m_meshSources[handle.index]}
               : std::nullopt;
}

std::optional<MaterialAssetSource> AssetManager::materialSource(MaterialHandle handle) const {
    return material(handle) != nullptr && handle.index < m_materialSources.size() &&
                   !m_materialSources[handle.index].path.empty()
               ? std::optional<MaterialAssetSource>{m_materialSources[handle.index]}
               : std::nullopt;
}

std::optional<TextureAssetSource> AssetManager::textureSource(TextureHandle handle) const {
    return texture(handle) != nullptr && handle.index < m_textureSources.size() &&
                   !m_textureSources[handle.index].path.empty()
               ? std::optional<TextureAssetSource>{m_textureSources[handle.index]}
               : std::nullopt;
}

ModelHandle AssetManager::addModel(ModelAsset asset, std::filesystem::path sourcePath) {
    ++m_revision;
    const uint64_t bytes = byteSize(asset);
    const ModelHandle handle =
        insertSlot<ModelAsset, ModelHandle>(m_models, m_modelMetadata, std::move(asset), bytes, ++m_useSerial);
    if (handle.index >= m_modelSources.size()) {
        m_modelSources.resize(handle.index + 1);
    }
    m_modelSources[handle.index] = {std::move(sourcePath)};
    return handle;
}

MeshHandle AssetManager::addMesh(MeshAsset asset, std::filesystem::path sourcePath, uint32_t meshIndex) {
    ++m_revision;
    asset.localBox = calculateBoundingBox(asset.vertices);
    const uint64_t bytes = byteSize(asset);
    const MeshHandle handle =
        insertSlot<MeshAsset, MeshHandle>(m_meshes, m_meshMetadata, std::move(asset), bytes, ++m_useSerial);
    m_meshMetadata[handle.index].gpuBytes = gpuByteSize(m_meshes[handle.index]);
    if (handle.index >= m_meshSources.size()) {
        m_meshSources.resize(handle.index + 1);
    }
    m_meshSources[handle.index] = {std::move(sourcePath), meshIndex};
    return handle;
}

MaterialHandle AssetManager::addMaterial(MaterialAsset asset) {
    ++m_revision;
    const uint64_t bytes = byteSize(asset);
    const MaterialHandle handle = insertSlot<MaterialAsset, MaterialHandle>(m_materials, m_materialMetadata,
                                                                            std::move(asset), bytes, ++m_useSerial);
    // A recycled slot must not keep the file of the material it held before;
    // loadMaterial sets one afterwards.
    if (handle.index >= m_materialSources.size()) {
        m_materialSources.resize(handle.index + 1);
    }
    m_materialSources[handle.index] = {};
    return handle;
}

TextureHandle AssetManager::addTexture(TextureAsset asset) {
    ++m_revision;
    const uint64_t bytes = byteSize(asset);
    const TextureHandle handle =
        insertSlot<TextureAsset, TextureHandle>(m_textures, m_textureMetadata, std::move(asset), bytes, ++m_useSerial);
    m_textureMetadata[handle.index].gpuBytes = gpuByteSize(m_textures[handle.index]);
    // A recycled slot must not keep the path of the texture it held before;
    // loadTexture sets one afterwards.
    if (handle.index < m_textureSources.size()) {
        m_textureSources[handle.index] = {};
    }
    return handle;
}

void AssetManager::setCookedTextureRoot(std::filesystem::path root) {
    m_cookedTextureRoot = std::move(root);
}

void AssetManager::setTextureFormatCapabilities(TextureFormatCapabilities capabilities) {
    m_textureCapabilities = capabilities;
}

uint64_t AssetManager::revision() const {
    return m_revision;
}

namespace {
template <typename Handle, typename Metadata>
AssetLease<Handle> makeLease(Handle handle, const std::vector<Metadata>& metadata) {
    if (!handle || handle.index >= metadata.size() || !metadata[handle.index].occupied ||
        metadata[handle.index].generation != handle.generation) {
        return {};
    }
    return AssetLease<Handle>(handle, std::make_shared<LeaseToken>(metadata[handle.index].references));
}
} // namespace

AssetLease<ModelHandle> AssetManager::lease(ModelHandle handle) const {
    AssetLease<ModelHandle> root = makeLease(handle, m_modelMetadata);
    const ModelAsset* asset = model(handle);
    if (!root || asset == nullptr) {
        return {};
    }
    auto composite = std::make_shared<CompositeLeaseToken>();
    composite->tokens.push_back(root.m_token);
    for (const ModelNode& node : asset->nodes) {
        AssetLease<MeshHandle> child = lease(node.mesh);
        if (child) {
            composite->tokens.push_back(child.m_token);
        }
    }
    return {handle, composite};
}

AssetLease<MeshHandle> AssetManager::lease(MeshHandle handle) const {
    AssetLease<MeshHandle> root = makeLease(handle, m_meshMetadata);
    const MeshAsset* asset = mesh(handle);
    if (!root || asset == nullptr) {
        return {};
    }
    auto composite = std::make_shared<CompositeLeaseToken>();
    composite->tokens.push_back(root.m_token);
    for (const MeshPrimitive& primitive : asset->primitives) {
        AssetLease<MaterialHandle> child = lease(primitive.material);
        if (child) {
            composite->tokens.push_back(child.m_token);
        }
    }
    return {handle, composite};
}

AssetLease<MaterialHandle> AssetManager::lease(MaterialHandle handle) const {
    AssetLease<MaterialHandle> root = makeLease(handle, m_materialMetadata);
    const MaterialAsset* asset = material(handle);
    if (!root || asset == nullptr) {
        return {};
    }
    auto composite = std::make_shared<CompositeLeaseToken>();
    composite->tokens.push_back(root.m_token);
    for (TextureHandle textureHandle : asset->textures()) {
        AssetLease<TextureHandle> child = lease(textureHandle);
        if (child) {
            composite->tokens.push_back(child.m_token);
        }
    }
    return {handle, composite};
}

AssetLease<TextureHandle> AssetManager::lease(TextureHandle handle) const {
    return makeLease(handle, m_textureMetadata);
}

AssetLease<FontHandle> AssetManager::lease(FontHandle handle) const {
    return makeLease(handle, m_fontMetadata);
}

AssetLeaseSet AssetManager::leaseSceneAssets(const Scene& scene) const {
    AssetLeaseSet result;
    const auto models = scene.registry().view<const ModelComponent>();
    for (Entity entity : models) {
        if (AssetLease<ModelHandle> value = lease(models.get<const ModelComponent>(entity).model)) {
            result.models.push_back(std::move(value));
        }
    }
    const auto modelNodes = scene.registry().view<const ModelNodeComponent>();
    for (Entity entity : modelNodes) {
        if (AssetLease<ModelHandle> value = lease(modelNodes.get<const ModelNodeComponent>(entity).model)) {
            result.models.push_back(std::move(value));
        }
    }
    const auto meshes = scene.registry().view<const MeshRendererComponent>();
    for (Entity entity : meshes) {
        if (AssetLease<MeshHandle> value = lease(meshes.get<const MeshRendererComponent>(entity).mesh)) {
            result.meshes.push_back(std::move(value));
        }
    }
    const auto overrides = scene.registry().view<const MaterialOverrideComponent>();
    for (Entity entity : overrides) {
        if (AssetLease<MaterialHandle> value = lease(overrides.get<const MaterialOverrideComponent>(entity).material)) {
            result.materials.push_back(std::move(value));
        }
    }
    const auto environments = scene.registry().view<const EnvironmentLightComponent>();
    for (Entity entity : environments) {
        if (AssetLease<TextureHandle> value =
                lease(environments.get<const EnvironmentLightComponent>(entity).environmentMap)) {
            result.textures.push_back(std::move(value));
        }
    }
    const auto probes = scene.registry().view<const ReflectionProbeComponent>();
    for (Entity entity : probes) {
        if (AssetLease<TextureHandle> value =
                lease(probes.get<const ReflectionProbeComponent>(entity).environmentMap)) {
            result.textures.push_back(std::move(value));
        }
    }
    const auto emitters = scene.registry().view<const ParticleEmitterComponent>();
    for (Entity entity : emitters) {
        if (AssetLease<TextureHandle> value = lease(emitters.get<const ParticleEmitterComponent>(entity).texture)) {
            result.textures.push_back(std::move(value));
        }
    }
    const auto decals = scene.registry().view<const DecalComponent>();
    for (Entity entity : decals) {
        const DecalComponent& decal = decals.get<const DecalComponent>(entity);
        for (TextureHandle texture : {decal.albedoTexture, decal.normalTexture}) {
            if (AssetLease<TextureHandle> value = lease(texture)) {
                result.textures.push_back(std::move(value));
            }
        }
    }
    return result;
}

void AssetManager::setMemoryBudget(AssetMemoryBudget budget) {
    m_budget = budget;
}

AssetMemoryBudget AssetManager::memoryBudget() const {
    return m_budget;
}

uint64_t AssetManager::cpuBytes() const {
    const auto sum = [](const auto& metadata) {
        return std::accumulate(metadata.begin(), metadata.end(), uint64_t{0}, [](uint64_t bytes, const auto& slot) {
            return bytes + (slot.occupied ? slot.cpuBytes : 0);
        });
    };
    return sum(m_modelMetadata) + sum(m_meshMetadata) + sum(m_materialMetadata) + sum(m_textureMetadata) +
           sum(m_fontMetadata);
}

AssetCollectionResult AssetManager::collectUnused(bool force) {
    AssetCollectionResult result;
    uint64_t retained = cpuBytes();
    const auto evict = [&](auto& assets, auto& metadata, AssetKind kind) {
        for (uint32_t index = 0; index < metadata.size(); ++index) {
            auto& slot = metadata[index];
            if (!slot.occupied || slot.references->load(std::memory_order_relaxed) != 0 ||
                (!force && retained <= m_budget.cpuBytes)) {
                continue;
            }
            retained -= slot.cpuBytes;
            result.bytesFreed += slot.cpuBytes;
            ++result.assetsEvicted;
            assets[index] = {};
            slot.occupied = false;
            slot.cpuBytes = 0;
            slot.gpuBytes = 0;
            ++slot.generation;
            if (slot.generation == 0) {
                slot.generation = 1;
            }
            ++m_revision;
            if (kind == AssetKind::Model) {
                std::erase_if(m_modelCache, [index](const auto& entry) { return entry.second.index == index; });
            } else if (kind == AssetKind::Font) {
                std::erase_if(m_fontCache, [index](const auto& entry) { return entry.second.index == index; });
            } else if (kind == AssetKind::Texture) {
                std::erase_if(m_textureCache, [index](const auto& entry) { return entry.second.index == index; });
            } else if (kind == AssetKind::Material) {
                std::erase_if(m_materialCache, [index](const auto& entry) { return entry.second.index == index; });
            }
        }
    };
    evict(m_models, m_modelMetadata, AssetKind::Model);
    evict(m_meshes, m_meshMetadata, AssetKind::Mesh);
    evict(m_materials, m_materialMetadata, AssetKind::Material);
    evict(m_textures, m_textureMetadata, AssetKind::Texture);
    evict(m_fonts, m_fontMetadata, AssetKind::Font);
    result.bytesRetained = retained;
    result.budgetSatisfied = retained <= m_budget.cpuBytes;
    return result;
}

std::vector<AssetDiagnostic> AssetManager::diagnostics() const {
    std::vector<AssetDiagnostic> result;
    const auto append = [&](const auto& metadata, AssetKind kind, const auto& sourceFor) {
        for (uint32_t index = 0; index < metadata.size(); ++index) {
            const auto& slot = metadata[index];
            if (slot.occupied) {
                // Runtime content has no source; its debug name stands in.
                std::string source = slot.debugName.empty() ? sourceFor(index) : "runtime:" + slot.debugName;
                result.push_back({kind, index, slot.generation, slot.references->load(std::memory_order_relaxed),
                                  slot.cpuBytes, slot.gpuBytes, std::move(source)});
            }
        }
    };
    append(m_modelMetadata, AssetKind::Model, [&](uint32_t index) {
        return index < m_modelSources.size() ? m_modelSources[index].path.generic_string() : std::string{};
    });
    append(m_meshMetadata, AssetKind::Mesh, [&](uint32_t index) {
        return index < m_meshSources.size() ? m_meshSources[index].path.generic_string() : std::string{};
    });
    append(m_materialMetadata, AssetKind::Material, [&](uint32_t index) {
        return index < m_materialSources.size() ? m_materialSources[index].path.generic_string() : std::string{};
    });
    append(m_textureMetadata, AssetKind::Texture, [&](uint32_t index) {
        return index < m_textureSources.size() ? m_textureSources[index].path.generic_string() : std::string{};
    });
    append(m_fontMetadata, AssetKind::Font, [](uint32_t) { return std::string{}; });
    return result;
}
