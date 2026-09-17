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
