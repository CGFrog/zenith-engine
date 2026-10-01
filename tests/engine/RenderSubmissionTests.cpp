#include "engine/graphics/RenderSubmission.hpp"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    std::vector<RenderBatch> batches(2000, {.indexCount = 36, .instanceCount = 1});
    RenderCapabilities unsupported;
    expect(buildIndirectPrototype(batches, unsupported).empty(),
           "the indirect prototype must remain behind all capability checks");
    expect(chooseRenderSubmissionPath(unsupported, {1.0, 0.1}) == RenderSubmissionPath::CompatibleInstanced,
           "unsupported devices retain the compatible path");

    RenderCapabilities supported{
        .descriptorIndexing = true, .multiDrawIndirect = true, .indirectDrawCount = true, .gpuDrivenPath = true};
    expect(buildIndirectPrototype(batches, supported).size() == 2000,
           "the capability-gated prototype should encode the 2,000-object workload");
    expect(chooseRenderSubmissionPath(supported, {1.0, 0.95}) == RenderSubmissionPath::CompatibleInstanced,
           "a non-material benchmark improvement must not select the prototype");
    expect(chooseRenderSubmissionPath(supported, {1.0, 0.80}) == RenderSubmissionPath::GpuDriven,
           "a measured material improvement may select the prototype");
    const std::vector<RenderOrderKey> keys{{RenderSurfaceClass::Blended, 4.0f},
                                           {RenderSurfaceClass::Opaque, 100.0f},
                                           {RenderSurfaceClass::Blended, 25.0f},
                                           {RenderSurfaceClass::Masked, 9.0f}};
    const std::vector<size_t> order = stableRenderOrder(keys);
    expect(order == std::vector<size_t>({1, 3, 2, 0}),
           "opaque and masked surfaces precede back-to-front blended surfaces");

    PersistentGpuScene scene;
    const GpuSceneObject first{
        .recordId = 10, .objectId = 10, .meshKey = 1, .materialKey = 2, .localBoundsRadius = 0.5f, .indexCount = 36};
    GpuSceneObject second = first;
    second.recordId = 11;
    second.objectId = 11;
    second.world[3].x = 0.5f;
    const GpuSceneHandle firstHandle = scene.upsert(first);
    const GpuSceneHandle secondHandle = scene.upsert(second);
    expect(scene.size() == 2 && scene.pendingDeltas().size() == 2,
           "persistent scene records create deltas without rebuilding existing objects");
    const GpuVisibilityResult initial =
        scene.buildVisibility({.depthHistoryValid = true, .occluded = [](const glm::vec3&, float) { return true; }});
    expect(initial.visibleInstances.size() == 2 && initial.commands.size() == 1,
           "new objects bypass occlusion and compatible objects form one indirect batch");
    const GpuVisibilityResult occluded =
        scene.buildVisibility({.depthHistoryValid = true, .occluded = [](const glm::vec3&, float) { return true; }});
    expect(occluded.visibleInstances.empty() && occluded.occlusionCulled == 2,
           "valid history culls persistent occluded objects");
    const GpuVisibilityResult cameraCut = scene.buildVisibility(
        {.depthHistoryValid = true, .cameraCut = true, .occluded = [](const glm::vec3&, float) { return true; }});
    expect(cameraCut.visibleInstances.size() == 2 && cameraCut.occlusionBypassed,
           "camera cuts bypass stale depth history for one frame");
    expect(scene.remove(firstHandle) && scene.resolve(firstHandle) == nullptr,
           "destroying an object invalidates its generation-checked handle");
    GpuSceneObject replacement = first;
    replacement.recordId = 12;
    replacement.objectId = 12;
    const GpuSceneHandle replacementHandle = scene.upsert(replacement);
    expect(replacementHandle.index == firstHandle.index && replacementHandle.generation != firstHandle.generation,
           "recycled GPU object slots advance their generation");
    expect(scene.resolve(secondHandle) != nullptr, "unrelated persistent objects remain valid");
    return failures == 0 ? 0 : 1;
}
