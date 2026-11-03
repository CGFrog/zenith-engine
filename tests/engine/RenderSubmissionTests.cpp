#include "engine/graphics/RenderSubmission.hpp"

#include "support/Doctest.hpp"

#include <vector>

namespace {

// A persistent GPU scene holding two compatible objects that share a mesh and material.
struct TwoObjectScene {
    TwoObjectScene() {
        second.recordId = 11;
        second.objectId = 11;
        second.world[3].x = 0.5f;
        firstHandle = scene.upsert(first);
        secondHandle = scene.upsert(second);
    }

    PersistentGpuScene scene;
    const GpuSceneObject first{
        .recordId = 10, .objectId = 10, .meshKey = 1, .materialKey = 2, .localBoundsRadius = 0.5f, .indexCount = 36};
    GpuSceneObject second = first;
    GpuSceneHandle firstHandle;
    GpuSceneHandle secondHandle;
};

} // namespace

TEST_SUITE_BEGIN("RenderSubmission");

TEST_CASE("RenderSubmission: the GPU-driven path needs every capability and a material measured gain") {
    std::vector<RenderBatch> batches(2000, {.indexCount = 36, .instanceCount = 1});
    RenderCapabilities unsupported;
    CHECK_MESSAGE(buildIndirectPrototype(batches, unsupported).empty(),
                  "the indirect prototype must remain behind all capability checks");
    CHECK_MESSAGE(chooseRenderSubmissionPath(unsupported, {1.0, 0.1}) == RenderSubmissionPath::CompatibleInstanced,
                  "unsupported devices retain the compatible path");

    RenderCapabilities supported{
        .descriptorIndexing = true, .multiDrawIndirect = true, .indirectDrawCount = true, .gpuDrivenPath = true};
    CHECK_MESSAGE(buildIndirectPrototype(batches, supported).size() == 2000,
                  "the capability-gated prototype should encode the 2,000-object workload");
    CHECK_MESSAGE(chooseRenderSubmissionPath(supported, {1.0, 0.95}) == RenderSubmissionPath::CompatibleInstanced,
                  "a non-material benchmark improvement must not select the prototype");
    CHECK_MESSAGE(chooseRenderSubmissionPath(supported, {1.0, 0.80}) == RenderSubmissionPath::GpuDriven,
                  "a measured material improvement may select the prototype");
}

TEST_CASE("RenderSubmission: opaque and masked surfaces precede back-to-front blended surfaces") {
    const std::vector<RenderOrderKey> keys{{RenderSurfaceClass::Blended, 4.0f},
                                           {RenderSurfaceClass::Opaque, 100.0f},
                                           {RenderSurfaceClass::Blended, 25.0f},
                                           {RenderSurfaceClass::Masked, 9.0f}};
    const std::vector<size_t> order = stableRenderOrder(keys);
    CHECK_MESSAGE((order == std::vector<size_t>({1, 3, 2, 0})),
                  "opaque and masked surfaces precede back-to-front blended surfaces");
}

TEST_CASE(
    "RenderSubmission: persistent scene visibility uses occlusion history except for new objects and camera cuts") {
    TwoObjectScene fixture;
    PersistentGpuScene& scene = fixture.scene;
    CHECK_MESSAGE((scene.size() == 2 && scene.pendingDeltas().size() == 2),
                  "persistent scene records create deltas without rebuilding existing objects");
    const GpuVisibilityResult initial =
        scene.buildVisibility({.depthHistoryValid = true, .occluded = [](const glm::vec3&, float) { return true; }});
    CHECK_MESSAGE((initial.visibleInstances.size() == 2 && initial.commands.size() == 1),
                  "new objects bypass occlusion and compatible objects form one indirect batch");
    const GpuVisibilityResult occluded =
        scene.buildVisibility({.depthHistoryValid = true, .occluded = [](const glm::vec3&, float) { return true; }});
    CHECK_MESSAGE((occluded.visibleInstances.empty() && occluded.occlusionCulled == 2),
                  "valid history culls persistent occluded objects");
    const GpuVisibilityResult cameraCut = scene.buildVisibility(
        {.depthHistoryValid = true, .cameraCut = true, .occluded = [](const glm::vec3&, float) { return true; }});
    CHECK_MESSAGE((cameraCut.visibleInstances.size() == 2 && cameraCut.occlusionBypassed),
                  "camera cuts bypass stale depth history for one frame");
}

TEST_CASE("RenderSubmission: persistent scene handles are generation checked") {
    TwoObjectScene fixture;
    PersistentGpuScene& scene = fixture.scene;
    const GpuSceneHandle firstHandle = fixture.firstHandle;
    CHECK_MESSAGE((scene.remove(firstHandle) && scene.resolve(firstHandle) == nullptr),
                  "destroying an object invalidates its generation-checked handle");
    GpuSceneObject replacement = fixture.first;
    replacement.recordId = 12;
    replacement.objectId = 12;
    const GpuSceneHandle replacementHandle = scene.upsert(replacement);
    CHECK_MESSAGE(
        (replacementHandle.index == firstHandle.index && replacementHandle.generation != firstHandle.generation),
        "recycled GPU object slots advance their generation");
    CHECK_MESSAGE(scene.resolve(fixture.secondHandle) != nullptr, "unrelated persistent objects remain valid");
}

TEST_CASE("RenderSubmission: indirect draw plans merge instances into batches and multi-draw groups") {
    // Instances of one primitive merge into a batch regardless of input order,
    // and batches sharing pipeline and page form one multi-draw group.
    const IndirectDrawKey crateSingle{.pipeline = 0, .geometryPage = 0, .firstIndex = 0, .indexCount = 36};
    const IndirectDrawKey barrelSingle{.pipeline = 0, .geometryPage = 0, .firstIndex = 36, .indexCount = 96};
    const IndirectDrawKey crateDouble{.pipeline = 1, .geometryPage = 0, .firstIndex = 0, .indexCount = 36};
    const IndirectDrawKey farPage{.pipeline = 0, .geometryPage = 1, .firstIndex = 0, .indexCount = 12};
    const std::vector<IndirectDrawKey> instances{crateDouble, barrelSingle, crateSingle, farPage, crateSingle};
    IndirectDrawPlan plan;
    buildIndirectDrawPlan(instances, plan);
    CHECK_MESSAGE(plan.batches.size() == 4, "identical primitives share one batch");
    CHECK_MESSAGE(plan.groups.size() == 3, "each pipeline and geometry page pair forms one draw group");
    CHECK_MESSAGE((plan.batches[0].key == crateSingle && plan.batches[0].instanceCount == 2 &&
                   plan.batches[0].firstInstance == 0),
                  "batches are ordered by key and own a dense instance range");
    CHECK_MESSAGE((plan.groups[0].batchCount == 2 && plan.groups[1].geometryPage == 1 && plan.groups[2].pipeline == 1),
                  "groups follow pipeline then geometry page order");
    bool slotsConsistent = plan.instanceOrder.size() == instances.size();
    for (uint32_t slot = 0; slotsConsistent && slot < plan.instanceOrder.size(); ++slot) {
        const IndirectDrawBatch& batch = plan.batches[plan.instanceBatch[slot]];
        slotsConsistent = instances[plan.instanceOrder[slot]] == batch.key && slot >= batch.firstInstance &&
                          slot < batch.firstInstance + batch.instanceCount;
    }
    CHECK_MESSAGE(slotsConsistent, "every instance slot lies inside the batch that matches its key");
    CHECK_MESSAGE((plan.instanceOrder[0] == 2 && plan.instanceOrder[1] == 4), "equal keys keep their input order");
    buildIndirectDrawPlan({}, plan);
    CHECK_MESSAGE((plan.batches.empty() && plan.groups.empty() && plan.instanceOrder.empty()),
                  "an empty scene produces an empty plan");
}

// The renderer checks the GPU-driven group capacity with the count before
// building a plan, and draws through CPU submission when it is exceeded.
TEST_CASE("RenderSubmission: indirect draw group count matches the plan") {
    std::vector<IndirectDrawKey> instances;
    for (uint32_t page = 0; page < 40; ++page) {
        for (uint32_t pipeline = 0; pipeline < 2; ++pipeline) {
            // Two primitives per pair, listed out of order, share one group.
            instances.push_back({.pipeline = pipeline, .geometryPage = 39 - page, .firstIndex = 36, .indexCount = 6});
            instances.push_back({.pipeline = pipeline, .geometryPage = page, .firstIndex = 0, .indexCount = 36});
        }
    }
    IndirectDrawPlan plan;
    buildIndirectDrawPlan(instances, plan);
    CHECK_MESSAGE((countIndirectDrawGroups(instances) == 80 && plan.groups.size() == 80),
                  "each distinct pipeline and geometry page pair counts once, as the plan groups them");
    CHECK_MESSAGE(countIndirectDrawGroups({}) == 0, "no instances need no groups");
}

TEST_SUITE_END();
