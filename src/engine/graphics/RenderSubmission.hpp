#pragma once

#include "Rendering.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

enum class RenderSubmissionPath {
    CompatibleInstanced,
    GpuDriven
};

enum class RenderSurfaceClass : uint8_t {
    Opaque,
    Masked,
    Blended,
};

struct RenderOrderKey {
    RenderSurfaceClass surface{RenderSurfaceClass::Opaque};
    float cameraDistanceSquared{0.0f};
};

struct RenderSubmissionBenchmark {
    double compatibleMilliseconds{0.0};
    double indirectMilliseconds{0.0};
};

struct RenderBatch {
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

struct IndirectDrawCommand {
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

struct GpuSceneHandle {
    uint32_t index{0};
    uint32_t generation{0};

    explicit operator bool() const {
        return generation != 0;
    }

    auto operator<=>(const GpuSceneHandle&) const = default;
};

struct GpuSceneObject {
    uint64_t recordId{0};
    RenderObjectId objectId{0};
    uint64_t meshKey{0};
    uint64_t materialKey{0};
    glm::mat4 world{1.0f};
    glm::vec3 localBoundsCenter{0.0f};
    float localBoundsRadius{0.0f};
    uint32_t indexCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
};

enum class GpuSceneDeltaType : uint8_t {
    Create,
    Update,
    Destroy,
};

struct GpuSceneDelta {
    GpuSceneDeltaType type{GpuSceneDeltaType::Create};
    GpuSceneHandle handle;
    std::optional<GpuSceneObject> object;
};

struct GpuVisibilityInput {
    glm::mat4 viewProjection{1.0f};
    bool depthHistoryValid{false};
    bool cameraCut{false};
    std::function<bool(const glm::vec3&, float)> occluded;
};

struct GpuVisibilityResult {
    std::vector<GpuSceneHandle> visibleInstances;
    std::vector<IndirectDrawCommand> commands;
    uint32_t frustumCulled{0};
    uint32_t occlusionCulled{0};
    bool occlusionBypassed{false};
};

class PersistentGpuScene {
  public:
    GpuSceneHandle upsert(const GpuSceneObject& object);
    bool update(GpuSceneHandle handle, const GpuSceneObject& object);
    bool remove(GpuSceneHandle handle);
    const GpuSceneObject* resolve(GpuSceneHandle handle) const;
    std::span<const GpuSceneDelta> pendingDeltas() const;
    void retireDeltas();
    GpuVisibilityResult buildVisibility(const GpuVisibilityInput& input);
    void buildVisibility(const GpuVisibilityInput& input, GpuVisibilityResult& result);
    size_t size() const;

  private:
    struct Slot {
        uint32_t generation{1};
        bool occupied{false};
        bool newlySpawned{false};
        GpuSceneObject object;
        glm::vec3 worldBoundsCenter{0.0f};
        float worldBoundsRadius{0.0f};
    };

    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_freeSlots;
    std::unordered_map<uint64_t, GpuSceneHandle> m_objects;
    std::vector<GpuSceneDelta> m_deltas;
    std::vector<uint32_t> m_visibleSlotScratch;
};

RenderSubmissionPath chooseRenderSubmissionPath(const RenderCapabilities& capabilities,
                                                const RenderSubmissionBenchmark& benchmark,
                                                double requiredImprovement = 0.10);
std::vector<IndirectDrawCommand> buildIndirectPrototype(std::span<const RenderBatch> batches,
                                                        const RenderCapabilities& capabilities);
std::vector<size_t> stableRenderOrder(std::span<const RenderOrderKey> keys);

// Identifies instances that can share one indexed draw. `pipeline` is a caller-
// defined bucket (for example single- versus double-sided); instances in the
// same pipeline and geometry page can be issued by a single multi-draw call.
struct IndirectDrawKey {
    uint32_t pipeline{0};
    uint32_t geometryPage{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t indexCount{0};

    auto operator<=>(const IndirectDrawKey&) const = default;
};

struct IndirectDrawBatch {
    IndirectDrawKey key;
    // Instance slots [firstInstance, firstInstance + instanceCount) belong to
    // this batch; culling fills them densely from the front.
    uint32_t firstInstance{0};
    uint32_t instanceCount{0};
    uint32_t group{0};
};

// A contiguous batch range sharing pipeline and geometry page.
struct IndirectDrawGroup {
    uint32_t pipeline{0};
    uint32_t geometryPage{0};
    uint32_t firstBatch{0};
    uint32_t batchCount{0};
};

struct IndirectDrawPlan {
    std::vector<IndirectDrawBatch> batches;
    std::vector<IndirectDrawGroup> groups;
    // instanceOrder[slot] is the input index stored at that instance slot.
    std::vector<uint32_t> instanceOrder;
    // instanceBatch[slot] is the batch owning that instance slot.
    std::vector<uint32_t> instanceBatch;
};

// Sorts instances by key into batches and groups. The result is deterministic
// for a given input, and existing vector capacity in `plan` is reused.
void buildIndirectDrawPlan(std::span<const IndirectDrawKey> instances, IndirectDrawPlan& plan);
