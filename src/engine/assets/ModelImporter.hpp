#pragma once

#include "Assets.hpp"

class ModelImporter {
  public:
    explicit ModelImporter(AssetManager& assets)
        : m_assets(assets) {}

    AssetLoadResult<ModelHandle> load(const std::filesystem::path& path);

  private:
    AssetManager& m_assets;
};

struct ModelTextureSource {
    std::filesystem::path path;
    TextureUsage usage{TextureUsage::Color};
};

struct ModelTextureSourcesResult {
    std::vector<ModelTextureSource> sources;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Lists a glTF model's external image files with the usage its materials give
// them. Embedded and data-URI images have no source file to cook and are
// omitted, as are normal maps also referenced as color or data (one cooked
// format cannot serve both, so those import as RGBA8).
ModelTextureSourcesResult collectModelTextureSources(const std::filesystem::path& modelPath);
