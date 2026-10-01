#include "Assets.hpp"
#include "ImageDecoder.hpp"
#include "ModelImporter.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
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
    return sizeof(TextureAsset) + asset.rgba.size() + asset.compressed.size() +
           asset.mips.size() * sizeof(TextureMipLevel);
}

uint64_t gpuByteSize(const TextureAsset& asset) {
    if (asset.blockCompressed()) {
        // Cooked textures upload their stored mip chain unchanged.
        return asset.compressed.size();
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
    return asset.vertices.size() * sizeof(Vertex) + asset.indices.size() * sizeof(uint32_t);
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
} // namespace

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

    std::ifstream input(normalized, std::ios::binary);
    if (!input) {
        return {.error = "Could not open font: " + normalized.string()};
    }
    FontAsset asset;
    asset.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
    const auto [normalized, key] = normalizedPath(path);
    if (const auto found = m_textureCache.find(key); found != m_textureCache.end()) {
        return {.handle = found->second};
    }
    ImageDecodeResult decoded = decodeImageFile(normalized);
    if (!decoded) {
        return {.error = decoded.error};
    }
    if (decoded.texture.blockCompressed()) {
        // Standalone textures feed CPU consumers such as environment
        // prefiltering, which read RGBA8 texels.
        return {.error = "Standalone textures must be RGBA8; block-compressed KTX2 is only supported for model "
                         "materials: " +
                         normalized.string()};
    }
    const TextureHandle handle = addTexture(std::move(decoded.texture));
    if (handle.index >= m_textureSources.size()) {
        m_textureSources.resize(handle.index + 1);
    }
    m_textureSources[handle.index] = {normalized};
    m_textureCache.emplace(key, handle);
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
    return mesh(handle) != nullptr && handle.index < m_meshSources.size()
               ? std::optional<MeshAssetSource>{m_meshSources[handle.index]}
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
    return handle;
}

TextureHandle AssetManager::addTexture(TextureAsset asset) {
    ++m_revision;
    const uint64_t bytes = byteSize(asset);
    const TextureHandle handle =
        insertSlot<TextureAsset, TextureHandle>(m_textures, m_textureMetadata, std::move(asset), bytes, ++m_useSerial);
    m_textureMetadata[handle.index].gpuBytes = gpuByteSize(m_textures[handle.index]);
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
    const std::array textures = {asset->baseColorTexture, asset->normalTexture, asset->metallicRoughnessTexture,
                                 asset->occlusionTexture, asset->emissiveTexture};
    for (TextureHandle textureHandle : textures) {
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
                result.push_back({kind, index, slot.generation, slot.references->load(std::memory_order_relaxed),
                                  slot.cpuBytes, slot.gpuBytes, sourceFor(index)});
            }
        }
    };
    append(m_modelMetadata, AssetKind::Model, [&](uint32_t index) {
        return index < m_modelSources.size() ? m_modelSources[index].path.generic_string() : std::string{};
    });
    append(m_meshMetadata, AssetKind::Mesh, [&](uint32_t index) {
        return index < m_meshSources.size() ? m_meshSources[index].path.generic_string() : std::string{};
    });
    append(m_materialMetadata, AssetKind::Material, [](uint32_t) { return std::string{}; });
    append(m_textureMetadata, AssetKind::Texture, [&](uint32_t index) {
        return index < m_textureSources.size() ? m_textureSources[index].path.generic_string() : std::string{};
    });
    append(m_fontMetadata, AssetKind::Font, [](uint32_t) { return std::string{}; });
    return result;
}
