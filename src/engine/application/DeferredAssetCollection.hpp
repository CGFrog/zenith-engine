#pragma once

// Runs asset collections requested at runtime once the frames in flight no longer use the assets.

#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"

#include <cstdint>
#include <optional>

struct DeferredAssetCollectionResult {
    AssetCollectionResult assets;
    AudioAssetCollectionResult audio;
};

// Runs asset collections requested at runtime (after a scene change, or when
// gameplay tore down part of its scene) at a point where they free something:
// once the frames that were in flight at the request have retired and dropped
// their asset leases. The host requests a collection, then reports each
// rendered frame; the collection runs on the framesToRetire-th frame after the
// latest request.
//
// Policy: every CPU asset nothing leases is evicted (AssetManager::
// collectUnused(true)); the scene, the renderer's frame slots, and anything
// else that must keep an asset hold leases. Decoded audio is collected only
// down to its budget (AudioSystem::collectUnusedAssets(false)), because clip
// handles held by idle emitters and gameplay code are not leased.
class DeferredAssetCollection {
  public:
    // framesToRetire: frames rendered after a request before it runs; the
    // renderer's Renderer::framesInFlight. Zero collects on the next report.
    explicit DeferredAssetCollection(uint32_t framesToRetire);

    // A request while one is pending starts its countdown over, so assets
    // released by the later request are unleased too when it runs.
    void request();
    bool pending() const;

    // Reports a rendered frame. Collects (audio may be null) and returns the
    // result when the pending request's countdown ends; nullopt otherwise.
    std::optional<DeferredAssetCollectionResult> frameRendered(AssetManager& assets, AudioSystem* audio);

  private:
    uint32_t m_framesToRetire{0};
    std::optional<uint32_t> m_framesRemaining;
};
