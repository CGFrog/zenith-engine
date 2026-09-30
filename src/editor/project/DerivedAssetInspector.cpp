#include "editor/project/DerivedAssetInspector.hpp"

DerivedAssetInspector::DerivedAssetInspector(std::filesystem::path projectRoot)
    : m_cache(std::move(projectRoot) / ".zenith" / "derived") {}

AssetPipelineResult DerivedAssetInspector::rebuild(const DerivedAssetRequest& request,
                                                   std::span<const uint8_t> derivedBytes, DerivedAssetRecord& record) {
    return m_cache.store(request, derivedBytes, record);
}

AssetPipelineResult DerivedAssetInspector::inspect(std::string_view key, DerivedAssetRecord& record) const {
    return m_cache.inspect(key, record);
}

AssetPipelineResult DerivedAssetInspector::invalidate(std::string_view key) {
    return m_cache.invalidate(key);
}

const std::filesystem::path& DerivedAssetInspector::cacheRoot() const {
    return m_cache.root();
}
