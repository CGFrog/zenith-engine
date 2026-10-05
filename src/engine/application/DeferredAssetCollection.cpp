#include "DeferredAssetCollection.hpp"

DeferredAssetCollection::DeferredAssetCollection(uint32_t framesToRetire)
    : m_framesToRetire(framesToRetire) {}

void DeferredAssetCollection::request() {
    m_framesRemaining = m_framesToRetire;
}

bool DeferredAssetCollection::pending() const {
    return m_framesRemaining.has_value();
}

std::optional<DeferredAssetCollectionResult> DeferredAssetCollection::frameRendered(AssetManager& assets,
                                                                                    AudioSystem* audio) {
    if (!m_framesRemaining) {
        return std::nullopt;
    }
    if (*m_framesRemaining > 1) {
        --*m_framesRemaining;
        return std::nullopt;
    }
    m_framesRemaining.reset();
    DeferredAssetCollectionResult result;
    result.assets = assets.collectUnused(true);
    if (audio != nullptr) {
        result.audio = audio->collectUnusedAssets(false);
    }
    return result;
}
