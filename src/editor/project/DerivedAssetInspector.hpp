#pragma once

#include "engine/assets/ProductionAssetPipeline.hpp"

class DerivedAssetInspector {
  public:
    explicit DerivedAssetInspector(std::filesystem::path projectRoot);
    AssetPipelineResult rebuild(const DerivedAssetRequest& request, std::span<const uint8_t> derivedBytes,
                                DerivedAssetRecord& record);
    AssetPipelineResult inspect(std::string_view key, DerivedAssetRecord& record) const;
    AssetPipelineResult invalidate(std::string_view key);
    const std::filesystem::path& cacheRoot() const;

  private:
    DerivedAssetCache m_cache;
};
