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

    // Instances of one primitive merge into a batch regardless of input order,
    // and batches sharing pipeline and page form one multi-draw group.
    const IndirectDrawKey crateSingle{.pipeline = 0, .geometryPage = 0, .firstIndex = 0, .indexCount = 36};
    const IndirectDrawKey barrelSingle{.pipeline = 0, .geometryPage = 0, .firstIndex = 36, .indexCount = 96};
    const IndirectDrawKey crateDouble{.pipeline = 1, .geometryPage = 0, .firstIndex = 0, .indexCount = 36};
    const IndirectDrawKey farPage{.pipeline = 0, .geometryPage = 1, .firstIndex = 0, .indexCount = 12};
    const std::vector<IndirectDrawKey> instances{crateDouble, barrelSingle, crateSingle, farPage, crateSingle};
    IndirectDrawPlan plan;
    buildIndirectDrawPlan(instances, plan);
    expect(plan.batches.size() == 4, "identical primitives share one batch");
    expect(plan.groups.size() == 3, "each pipeline and geometry page pair forms one draw group");
    expect(plan.batches[0].key == crateSingle && plan.batches[0].instanceCount == 2 &&
               plan.batches[0].firstInstance == 0,
           "batches are ordered by key and own a dense instance range");
    expect(plan.groups[0].batchCount == 2 && plan.groups[1].geometryPage == 1 && plan.groups[2].pipeline == 1,
           "groups follow pipeline then geometry page order");
    bool slotsConsistent = plan.instanceOrder.size() == instances.size();
    for (uint32_t slot = 0; slotsConsistent && slot < plan.instanceOrder.size(); ++slot) {
        const IndirectDrawBatch& batch = plan.batches[plan.instanceBatch[slot]];
        slotsConsistent = instances[plan.instanceOrder[slot]] == batch.key && slot >= batch.firstInstance &&
                          slot < batch.firstInstance + batch.instanceCount;
    }
    expect(slotsConsistent, "every instance slot lies inside the batch that matches its key");
    expect(plan.instanceOrder[0] == 2 && plan.instanceOrder[1] == 4, "equal keys keep their input order");
    buildIndirectDrawPlan({}, plan);
    expect(plan.batches.empty() && plan.groups.empty() && plan.instanceOrder.empty(),
           "an empty scene produces an empty plan");
    return failures == 0 ? 0 : 1;
}
