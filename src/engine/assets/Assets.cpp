#include "Assets.hpp"
#include "ModelImporter.hpp"

#include <fstream>
#include <iterator>
#include <utility>

namespace {
template <typename T, typename Handle> const T* resolve(const std::vector<T>& assets, Handle handle) {
    return handle && handle.generation == 1 && handle.index < assets.size() ? &assets[handle.index] : nullptr;
}

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
    const size_t modelCount = m_models.size();
    const size_t meshCount = m_meshes.size();
    const size_t materialCount = m_materials.size();
    const size_t textureCount = m_textures.size();

    ModelImporter importer(*this);
    auto result = importer.load(path);

    if (result) {
        m_modelCache.insert_or_assign(cacheKey, result.handle);
    } else {
        m_models.resize(modelCount);
        m_modelSources.resize(modelCount);
        m_meshes.resize(meshCount);
        m_meshSources.resize(meshCount);
        m_materials.resize(materialCount);
        m_textures.resize(textureCount);
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
            return {.handle = MeshHandle{static_cast<uint32_t>(index), 1}};
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
    const FontHandle handle{static_cast<uint32_t>(m_fonts.size()), 1};
    m_fonts.push_back(std::move(asset));
    m_fontCache.emplace(key, handle);
    return {.handle = handle};
}

const ModelAsset* AssetManager::model(ModelHandle handle) const {
    return resolve(m_models, handle);
}

const MeshAsset* AssetManager::mesh(MeshHandle handle) const {
    return resolve(m_meshes, handle);
}

const MaterialAsset* AssetManager::material(MaterialHandle handle) const {
    return resolve(m_materials, handle);
}

const TextureAsset* AssetManager::texture(TextureHandle handle) const {
    return resolve(m_textures, handle);
}

const FontAsset* AssetManager::font(FontHandle handle) const {
    return resolve(m_fonts, handle);
}

size_t AssetManager::fontCount() const {
    return m_fonts.size();
}

std::optional<ModelAssetSource> AssetManager::modelSource(ModelHandle handle) const {
    return handle && handle.generation == 1 && handle.index < m_modelSources.size()
               ? std::optional<ModelAssetSource>{m_modelSources[handle.index]}
               : std::nullopt;
}

std::optional<MeshAssetSource> AssetManager::meshSource(MeshHandle handle) const {
    return handle && handle.generation == 1 && handle.index < m_meshSources.size()
               ? std::optional<MeshAssetSource>{m_meshSources[handle.index]}
               : std::nullopt;
}

ModelHandle AssetManager::addModel(ModelAsset asset, std::filesystem::path sourcePath) {
    ++m_revision;
    const ModelHandle handle{static_cast<uint32_t>(m_models.size()), 1};
    m_models.push_back(std::move(asset));
    m_modelSources.push_back({std::move(sourcePath)});
    return handle;
}

MeshHandle AssetManager::addMesh(MeshAsset asset, std::filesystem::path sourcePath, uint32_t meshIndex) {
    ++m_revision;
    const MeshHandle handle{static_cast<uint32_t>(m_meshes.size()), 1};
    m_meshes.push_back(std::move(asset));
    m_meshSources.push_back({std::move(sourcePath), meshIndex});
    return handle;
}

MaterialHandle AssetManager::addMaterial(MaterialAsset asset) {
    ++m_revision;
    const MaterialHandle handle{static_cast<uint32_t>(m_materials.size()), 1};
    m_materials.push_back(std::move(asset));
    return handle;
}

TextureHandle AssetManager::addTexture(TextureAsset asset) {
    ++m_revision;
    const TextureHandle handle{static_cast<uint32_t>(m_textures.size()), 1};
    m_textures.push_back(std::move(asset));
    return handle;
}

uint64_t AssetManager::revision() const {
    return m_revision;
}
