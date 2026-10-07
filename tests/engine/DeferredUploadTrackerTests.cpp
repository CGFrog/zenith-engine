#include "engine/graphics/DeferredUploadTracker.hpp"

#include "support/Doctest.hpp"

#include <cstdint>
#include <iostream>
#include <string>

TEST_SUITE_BEGIN("DeferredUploadTracker");

namespace {

constexpr uint64_t meshKey = (uint64_t{2} << 32) | 7u;
constexpr uint64_t textureKey = (uint64_t{1} << 32) | 3u;

TEST_CASE("DeferredUploadTracker: age increments once per frame") {
    DeferredUploadTracker tracker({.warningFrames = 10});
    for (uint32_t frame = 1; frame <= 3; ++frame) {
        // Several requests in one frame (shared mesh) still age it once.
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        CHECK_MESSAGE(tracker.deferredThisFrame(DeferredUploadKind::Mesh, meshKey),
                      "deferral is visible in the open frame");
        const DeferredUploadFrameSummary summary = tracker.endFrame();
        CHECK_MESSAGE(summary.deferredMeshes == 1, "a mesh deferred twice in one frame counts once");
        CHECK_MESSAGE(summary.deferredTextures == 0, "mesh deferrals do not count as textures");
        CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == frame, "age grows by one per frame");
    }
    CHECK_MESSAGE(!tracker.deferredThisFrame(DeferredUploadKind::Mesh, meshKey), "endFrame closes the frame");
    CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Texture, meshKey) == 0, "kinds are tracked independently");
}

TEST_CASE("DeferredUploadTracker: warns once past bound") {
    DeferredUploadTracker tracker({.warningFrames = 4});
    uint32_t warnings = 0;
    for (uint32_t frame = 1; frame <= 20; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
        const DeferredUploadFrameSummary summary = tracker.endFrame();
        for (const DeferredUploadWarning& warning : summary.overdue) {
            ++warnings;
            CHECK_MESSAGE(frame == 5, "the warning fires on the first frame past the bound");
            CHECK_MESSAGE((warning.kind == DeferredUploadKind::Texture && warning.assetKey == textureKey &&
                           warning.deferredFrames == 5),
                          "the warning identifies the asset and its age");
            const std::string text = describeDeferredUpload(warning);
            CHECK_MESSAGE((text.find("Texture") != std::string::npos && text.find("index 3") != std::string::npos &&
                           text.find("generation 1") != std::string::npos &&
                           text.find("5 consecutive") != std::string::npos),
                          "the warning text is descriptive");
        }
    }
    CHECK_MESSAGE(warnings == 1, "an asset that stays deferred warns exactly once");
}

TEST_CASE("DeferredUploadTracker: zero bound disables warnings") {
    DeferredUploadTracker tracker({.warningFrames = 0});
    for (int frame = 0; frame < 300; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        CHECK_MESSAGE(tracker.endFrame().overdue.empty(), "a zero bound never warns");
    }
    CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 300, "a zero bound still counts");
}

TEST_CASE("DeferredUploadTracker: upload resets age") {
    DeferredUploadTracker tracker({.warningFrames = 2});
    for (int frame = 0; frame < 5; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        tracker.endFrame();
    }
    tracker.markUploaded(DeferredUploadKind::Mesh, meshKey);
    CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 0, "an upload forgets the asset");
    CHECK_MESSAGE(tracker.trackedAssetCount() == 0, "uploaded assets free their entry");
    const DeferredUploadFrameSummary landed = tracker.endFrame();
    CHECK_MESSAGE(landed.deferredMeshes == 0, "an uploaded mesh is no longer counted");

    // A new deferral episode starts at age zero and may warn again.
    uint32_t warnings = 0;
    for (int frame = 0; frame < 3; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        warnings += static_cast<uint32_t>(tracker.endFrame().overdue.size());
    }
    CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 3, "a new episode restarts the age");
    CHECK_MESSAGE(warnings == 1, "a new episode past the bound warns again");

    // Deferred then uploaded within the same frame does not count.
    tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
    tracker.markUploaded(DeferredUploadKind::Texture, textureKey);
    CHECK_MESSAGE(tracker.endFrame().deferredTextures == 0, "an upload that lands in the same frame is not counted");
}

TEST_CASE("DeferredUploadTracker: release and unrequested assets are forgotten") {
    DeferredUploadTracker tracker({.warningFrames = 2});
    tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
    tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
    tracker.endFrame();
    CHECK_MESSAGE(tracker.trackedAssetCount() == 2, "both deferred assets are tracked");

    tracker.release(DeferredUploadKind::Mesh, meshKey);
    CHECK_MESSAGE(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 0, "release forgets the asset");

    // The texture is not requested this frame (released or no longer drawn).
    const DeferredUploadFrameSummary summary = tracker.endFrame();
    CHECK_MESSAGE((summary.deferredMeshes == 0 && summary.deferredTextures == 0), "nothing was deferred this frame");
    CHECK_MESSAGE(tracker.trackedAssetCount() == 0, "unrequested assets are forgotten, bounding memory");

    // Many distinct assets deferred once do not accumulate.
    for (uint64_t key = 0; key < 1000; ++key) {
        tracker.markDeferred(DeferredUploadKind::Texture, key);
    }
    CHECK_MESSAGE(tracker.endFrame().deferredTextures == 1000, "each distinct deferred asset is counted");
    tracker.endFrame();
    CHECK_MESSAGE(tracker.trackedAssetCount() == 0, "entries are dropped once assets stop being deferred");
}
} // namespace

TEST_SUITE_END();
