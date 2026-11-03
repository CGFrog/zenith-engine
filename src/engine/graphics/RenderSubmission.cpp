#include "RenderSubmission.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <tuple>

namespace {
struct Plane {
    glm::vec3 normal{0.0f};
    float distance{0.0f};
};

std::array<Plane, 6> frustumPlanes(const glm::mat4& matrix) {
    const glm::mat4 transposed = glm::transpose(matrix);
    const std::array<glm::vec4, 6> equations{transposed[3] + transposed[0], transposed[3] - transposed[0],
                                             transposed[3] + transposed[1], transposed[3] - transposed[1],
                                             transposed[3] + transposed[2], transposed[3] - transposed[2]};
    std::array<Plane, 6> result;
    for (size_t index = 0; index < equations.size(); ++index) {
        const float length = glm::length(glm::vec3(equations[index]));
        result[index] = {glm::vec3(equations[index]) / length, equations[index].w / length};
    }
    return result;
}

bool visible(const std::array<Plane, 6>& planes, const glm::vec3& center, float radius) {
    return std::ranges::all_of(
        planes, [&](const Plane& plane) { return glm::dot(plane.normal, center) + plane.distance >= -radius; });
}

std::pair<glm::vec3, float> worldBounds(const GpuSceneObject& object) {
    const glm::vec3 center = glm::vec3(object.world * glm::vec4(object.localBoundsCenter, 1.0f));
    const float scale = std::max({glm::length(glm::vec3(object.world[0])), glm::length(glm::vec3(object.world[1])),
                                  glm::length(glm::vec3(object.world[2]))});
    return {center, object.localBoundsRadius * scale};
}
} // namespace

RenderSubmissionPath chooseRenderSubmissionPath(const RenderCapabilities& capabilities,
                                                const RenderSubmissionBenchmark& benchmark,
                                                double requiredImprovement) {
    const bool supported =
        capabilities.gpuDrivenPath && capabilities.descriptorIndexing && capabilities.multiDrawIndirect;
    if (!supported || benchmark.compatibleMilliseconds <= 0.0 || benchmark.indirectMilliseconds <= 0.0) {
        return RenderSubmissionPath::CompatibleInstanced;
    }
    const double improvement =
        (benchmark.compatibleMilliseconds - benchmark.indirectMilliseconds) / benchmark.compatibleMilliseconds;
    return improvement >= requiredImprovement ? RenderSubmissionPath::GpuDriven
                                              : RenderSubmissionPath::CompatibleInstanced;
}

GpuSceneHandle PersistentGpuScene::upsert(const GpuSceneObject& object) {
    if (const auto found = m_objects.find(object.recordId); found != m_objects.end()) {
        update(found->second, object);
        return found->second;
    }
    uint32_t index = 0;
    if (m_freeSlots.empty()) {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    } else {
        index = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    Slot& slot = m_slots[index];
    slot.occupied = true;
    slot.newlySpawned = true;
    slot.object = object;
    std::tie(slot.worldBoundsCenter, slot.worldBoundsRadius) = worldBounds(object);
    const GpuSceneHandle handle{index, slot.generation};
    m_objects[object.recordId] = handle;
    m_deltas.push_back({GpuSceneDeltaType::Create, handle, object});
    return handle;
}

bool PersistentGpuScene::update(GpuSceneHandle handle, const GpuSceneObject& object) {
    if (handle.index >= m_slots.size()) {
        return false;
    }
    Slot& slot = m_slots[handle.index];
    if (!slot.occupied || slot.generation != handle.generation || slot.object.recordId != object.recordId) {
        return false;
    }
    slot.object = object;
    std::tie(slot.worldBoundsCenter, slot.worldBoundsRadius) = worldBounds(object);
    m_deltas.push_back({GpuSceneDeltaType::Update, handle, object});
    return true;
}

bool PersistentGpuScene::remove(GpuSceneHandle handle) {
    if (handle.index >= m_slots.size()) {
        return false;
    }
    Slot& slot = m_slots[handle.index];
    if (!slot.occupied || slot.generation != handle.generation) {
        return false;
    }
    m_objects.erase(slot.object.recordId);
    m_deltas.push_back({GpuSceneDeltaType::Destroy, handle, std::nullopt});
    slot.occupied = false;
    slot.newlySpawned = false;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    m_freeSlots.push_back(handle.index);
    return true;
}

const GpuSceneObject* PersistentGpuScene::resolve(GpuSceneHandle handle) const {
    if (handle.index >= m_slots.size()) {
        return nullptr;
    }
    const Slot& slot = m_slots[handle.index];
    return slot.occupied && slot.generation == handle.generation ? &slot.object : nullptr;
}

std::span<const GpuSceneDelta> PersistentGpuScene::pendingDeltas() const {
    return m_deltas;
}

void PersistentGpuScene::retireDeltas() {
    m_deltas.clear();
}

GpuVisibilityResult PersistentGpuScene::buildVisibility(const GpuVisibilityInput& input) {
    GpuVisibilityResult result;
    buildVisibility(input, result);
    return result;
}

void PersistentGpuScene::buildVisibility(const GpuVisibilityInput& input, GpuVisibilityResult& result) {
    result.visibleInstances.clear();
    result.commands.clear();
    result.frustumCulled = 0;
    result.occlusionCulled = 0;
    const std::array<Plane, 6> planes = frustumPlanes(input.viewProjection);
    result.occlusionBypassed = input.cameraCut || !input.depthHistoryValid || !input.occluded;

    m_visibleSlotScratch.clear();
    m_visibleSlotScratch.reserve(std::max(m_visibleSlotScratch.capacity(), m_objects.size()));
    for (uint32_t index = 0; index < m_slots.size(); ++index) {
        Slot& slot = m_slots[index];
        if (!slot.occupied) {
            continue;
        }
        const glm::vec3& center = slot.worldBoundsCenter;
        const float radius = slot.worldBoundsRadius;
        if (!visible(planes, center, radius)) {
            ++result.frustumCulled;
            slot.newlySpawned = false;
            continue;
        }
        if (!result.occlusionBypassed && !slot.newlySpawned && input.occluded(center, radius)) {
            ++result.occlusionCulled;
            continue;
        }
        m_visibleSlotScratch.push_back(index);
        slot.newlySpawned = false;
    }
    std::ranges::sort(m_visibleSlotScratch, [&](uint32_t first, uint32_t second) {
        const GpuSceneObject& firstObject = m_slots[first].object;
        const GpuSceneObject& secondObject = m_slots[second].object;
        return std::tie(firstObject.materialKey, firstObject.meshKey, firstObject.firstIndex, firstObject.objectId) <
               std::tie(secondObject.materialKey, secondObject.meshKey, secondObject.firstIndex, secondObject.objectId);
    });
    for (size_t begin = 0; begin < m_visibleSlotScratch.size();) {
        size_t end = begin + 1;
        const GpuSceneObject& object = m_slots[m_visibleSlotScratch[begin]].object;
        while (end < m_visibleSlotScratch.size()) {
            const GpuSceneObject& candidate = m_slots[m_visibleSlotScratch[end]].object;
            if (candidate.materialKey != object.materialKey || candidate.meshKey != object.meshKey ||
                candidate.firstIndex != object.firstIndex || candidate.indexCount != object.indexCount ||
                candidate.vertexOffset != object.vertexOffset) {
                break;
            }
            ++end;
        }
        const uint32_t firstInstance = static_cast<uint32_t>(result.visibleInstances.size());
        for (size_t index = begin; index < end; ++index) {
            const uint32_t slotIndex = m_visibleSlotScratch[index];
            result.visibleInstances.push_back({slotIndex, m_slots[slotIndex].generation});
        }
        result.commands.push_back({object.indexCount, static_cast<uint32_t>(end - begin), object.firstIndex,
                                   object.vertexOffset, firstInstance});
        begin = end;
    }
}

size_t PersistentGpuScene::size() const {
    return m_objects.size();
}

std::vector<IndirectDrawCommand> buildIndirectPrototype(std::span<const RenderBatch> batches,
                                                        const RenderCapabilities& capabilities) {
    if (!capabilities.gpuDrivenPath || !capabilities.descriptorIndexing || !capabilities.multiDrawIndirect) {
        return {};
    }
    std::vector<IndirectDrawCommand> commands;
    commands.reserve(batches.size());
    for (const RenderBatch& batch : batches) {
        commands.push_back(
            {batch.indexCount, batch.instanceCount, batch.firstIndex, batch.vertexOffset, batch.firstInstance});
    }
    return commands;
}

std::vector<size_t> stableRenderOrder(std::span<const RenderOrderKey> keys) {
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [keys](size_t first, size_t second) {
        const bool firstBlended = keys[first].surface == RenderSurfaceClass::Blended;
        const bool secondBlended = keys[second].surface == RenderSurfaceClass::Blended;
        if (firstBlended != secondBlended) {
            return !firstBlended;
        }
        return firstBlended && keys[first].cameraDistanceSquared > keys[second].cameraDistanceSquared;
    });
    return order;
}

uint32_t countIndirectDrawGroups(std::span<const IndirectDrawKey> instances) {
    // Sorted; scenes use few groups, so inserting stays cheap.
    std::vector<std::pair<uint32_t, uint32_t>> groups;
    for (const IndirectDrawKey& key : instances) {
        const std::pair<uint32_t, uint32_t> group(key.pipeline, key.geometryPage);
        const auto found = std::ranges::lower_bound(groups, group);
        if (found == groups.end() || *found != group) {
            groups.insert(found, group);
        }
    }
    return static_cast<uint32_t>(groups.size());
}

void buildIndirectDrawPlan(std::span<const IndirectDrawKey> instances, IndirectDrawPlan& plan) {
    plan.batches.clear();
    plan.groups.clear();
    plan.instanceOrder.resize(instances.size());
    plan.instanceBatch.resize(instances.size());
    std::iota(plan.instanceOrder.begin(), plan.instanceOrder.end(), 0u);
    std::ranges::stable_sort(plan.instanceOrder, [instances](uint32_t first, uint32_t second) {
        return instances[first] < instances[second];
    });
    for (uint32_t slot = 0; slot < plan.instanceOrder.size(); ++slot) {
        const IndirectDrawKey& key = instances[plan.instanceOrder[slot]];
        if (plan.batches.empty() || plan.batches.back().key != key) {
            const bool newGroup = plan.groups.empty() || plan.groups.back().pipeline != key.pipeline ||
                                  plan.groups.back().geometryPage != key.geometryPage;
            if (newGroup) {
                plan.groups.push_back({.pipeline = key.pipeline,
                                       .geometryPage = key.geometryPage,
                                       .firstBatch = static_cast<uint32_t>(plan.batches.size())});
            }
            plan.batches.push_back(
                {.key = key, .firstInstance = slot, .group = static_cast<uint32_t>(plan.groups.size() - 1)});
            ++plan.groups.back().batchCount;
        }
        ++plan.batches.back().instanceCount;
        plan.instanceBatch[slot] = static_cast<uint32_t>(plan.batches.size() - 1);
    }
}
