#include "Assets.hpp"
#include "ModelImporter.hpp"

#include <utility>

namespace {
template <typename T, typename Handle> const T* resolve(const std::vector<T>& assets, Handle handle) {
    return handle && handle.generation == 1 && handle.index < assets.size() ? &assets[handle.index] : nullptr;
}
} // namespace

AssetLoadResult<ModelHandle> AssetManager::loadModel(const std::filesystem::path& path) {
    std::error_code ec;
    const auto normalized = std::filesystem::weakly_canonical(path, ec);
    const std::string key = (ec ? path.lexically_normal() : normalized).generic_string();
    if (const auto found = m_modelCache.find(key); found != m_modelCache.end()) {
        return {.handle = found->second};
    }

    ModelImporter importer(*this);
    auto result = importer.load(path);

    if (result) {
        m_modelCache.emplace(key, result.handle);
    }

    return result;
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

ModelHandle AssetManager::addModel(ModelAsset asset) {
    const ModelHandle handle{static_cast<uint32_t>(m_models.size()), 1};
    m_models.push_back(std::move(asset));
    return handle;
}

MeshHandle AssetManager::addMesh(MeshAsset asset) {
    const MeshHandle handle{static_cast<uint32_t>(m_meshes.size()), 1};
    m_meshes.push_back(std::move(asset));
    return handle;
}

MaterialHandle AssetManager::addMaterial(MaterialAsset asset) {
    const MaterialHandle handle{static_cast<uint32_t>(m_materials.size()), 1};
    m_materials.push_back(std::move(asset));
    return handle;
}

TextureHandle AssetManager::addTexture(TextureAsset asset) {
    const TextureHandle handle{static_cast<uint32_t>(m_textures.size()), 1};
    m_textures.push_back(std::move(asset));
    return handle;
}
