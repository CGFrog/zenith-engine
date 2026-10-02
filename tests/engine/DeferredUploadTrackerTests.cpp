#include "engine/graphics/DeferredUploadTracker.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr uint64_t meshKey = (uint64_t{2} << 32) | 7u;
constexpr uint64_t textureKey = (uint64_t{1} << 32) | 3u;

void testAgeIncrementsOncePerFrame() {
    DeferredUploadTracker tracker({.warningFrames = 10});
    for (uint32_t frame = 1; frame <= 3; ++frame) {
        // Several requests in one frame (shared mesh) still age it once.
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        expect(tracker.deferredThisFrame(DeferredUploadKind::Mesh, meshKey), "deferral is visible in the open frame");
        const DeferredUploadFrameSummary summary = tracker.endFrame();
        expect(summary.deferredMeshes == 1, "a mesh deferred twice in one frame counts once");
        expect(summary.deferredTextures == 0, "mesh deferrals do not count as textures");
        expect(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == frame, "age grows by one per frame");
    }
    expect(!tracker.deferredThisFrame(DeferredUploadKind::Mesh, meshKey), "endFrame closes the frame");
    expect(tracker.deferredFrames(DeferredUploadKind::Texture, meshKey) == 0, "kinds are tracked independently");
}

void testWarnsOncePastBound() {
    DeferredUploadTracker tracker({.warningFrames = 4});
    uint32_t warnings = 0;
    for (uint32_t frame = 1; frame <= 20; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
        const DeferredUploadFrameSummary summary = tracker.endFrame();
        for (const DeferredUploadWarning& warning : summary.overdue) {
            ++warnings;
            expect(frame == 5, "the warning fires on the first frame past the bound");
            expect(warning.kind == DeferredUploadKind::Texture && warning.assetKey == textureKey &&
                       warning.deferredFrames == 5,
                   "the warning identifies the asset and its age");
            const std::string text = describeDeferredUpload(warning);
            expect(text.find("Texture") != std::string::npos && text.find("index 3") != std::string::npos &&
                       text.find("generation 1") != std::string::npos &&
                       text.find("5 consecutive") != std::string::npos,
                   "the warning text is descriptive");
        }
    }
    expect(warnings == 1, "an asset that stays deferred warns exactly once");
}

void testZeroBoundDisablesWarnings() {
    DeferredUploadTracker tracker({.warningFrames = 0});
    for (int frame = 0; frame < 300; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        expect(tracker.endFrame().overdue.empty(), "a zero bound never warns");
    }
    expect(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 300, "a zero bound still counts");
}

void testUploadResetsAge() {
    DeferredUploadTracker tracker({.warningFrames = 2});
    for (int frame = 0; frame < 5; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        tracker.endFrame();
    }
    tracker.markUploaded(DeferredUploadKind::Mesh, meshKey);
    expect(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 0, "an upload forgets the asset");
    expect(tracker.trackedAssetCount() == 0, "uploaded assets free their entry");
    const DeferredUploadFrameSummary landed = tracker.endFrame();
    expect(landed.deferredMeshes == 0, "an uploaded mesh is no longer counted");

    // A new deferral episode starts at age zero and may warn again.
    uint32_t warnings = 0;
    for (int frame = 0; frame < 3; ++frame) {
        tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
        warnings += static_cast<uint32_t>(tracker.endFrame().overdue.size());
    }
    expect(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 3, "a new episode restarts the age");
    expect(warnings == 1, "a new episode past the bound warns again");

    // Deferred then uploaded within the same frame does not count.
    tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
    tracker.markUploaded(DeferredUploadKind::Texture, textureKey);
    expect(tracker.endFrame().deferredTextures == 0, "an upload that lands in the same frame is not counted");
}

void testReleaseAndUnrequestedAssetsAreForgotten() {
    DeferredUploadTracker tracker({.warningFrames = 2});
    tracker.markDeferred(DeferredUploadKind::Mesh, meshKey);
    tracker.markDeferred(DeferredUploadKind::Texture, textureKey);
    tracker.endFrame();
    expect(tracker.trackedAssetCount() == 2, "both deferred assets are tracked");

    tracker.release(DeferredUploadKind::Mesh, meshKey);
    expect(tracker.deferredFrames(DeferredUploadKind::Mesh, meshKey) == 0, "release forgets the asset");

    // The texture is not requested this frame (released or no longer drawn).
    const DeferredUploadFrameSummary summary = tracker.endFrame();
    expect(summary.deferredMeshes == 0 && summary.deferredTextures == 0, "nothing was deferred this frame");
    expect(tracker.trackedAssetCount() == 0, "unrequested assets are forgotten, bounding memory");

    // Many distinct assets deferred once do not accumulate.
    for (uint64_t key = 0; key < 1000; ++key) {
        tracker.markDeferred(DeferredUploadKind::Texture, key);
    }
    expect(tracker.endFrame().deferredTextures == 1000, "each distinct deferred asset is counted");
    tracker.endFrame();
    expect(tracker.trackedAssetCount() == 0, "entries are dropped once assets stop being deferred");
}
} // namespace

int main() {
    testAgeIncrementsOncePerFrame();
    testWarnsOncePastBound();
    testZeroBoundDisablesWarnings();
    testUploadResetsAge();
    testReleaseAndUnrequestedAssetsAreForgotten();
    if (failures != 0) {
        std::cerr << failures << " deferred upload tracker check(s) failed\n";
        return 1;
    }
    return 0;
}
