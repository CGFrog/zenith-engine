#include "Particles.hpp"

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace particles {
namespace {
// Compare-exchange of a bitonic step: ascending pairs end up ordered low to
// high, descending ones high to low. Mirrors compareExchange in particle_sort.comp.
void compareExchange(std::span<SortKey> keys, uint32_t first, uint32_t second, bool ascending) {
    if ((keys[first] > keys[second]) == ascending) {
        std::swap(keys[first], keys[second]);
    }
}

// Lower index of the pair a thread handles in a step of compare distance j.
uint32_t pairIndex(uint32_t thread, uint32_t distance) {
    return 2 * distance * (thread / distance) + thread % distance;
}

// The block steps of particle_sort.comp: every thread of the block's
// workgroup handles one pair per step.
void mergeInsideBlock(std::span<SortKey> keys, uint32_t blockStart, uint32_t mergeSize, uint32_t firstDistance) {
    for (uint32_t distance = firstDistance; distance > 0; distance /= 2) {
        for (uint32_t thread = 0; thread < sortWorkgroupSize; ++thread) {
            const uint32_t first = blockStart + pairIndex(thread, distance);
            compareExchange(keys, first, first + distance, (first & mergeSize) == 0);
        }
    }
}

float clampedDeltaSeconds(float deltaSeconds) {
    return std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f;
}
} // namespace

uint32_t particleBudget(ParticleQuality quality) {
    switch (quality) {
    case ParticleQuality::Off:
        return 0;
    case ParticleQuality::Low:
        return 16384;
    case ParticleQuality::High:
        return 131072;
    }
    return 0;
}

uint32_t backToFrontKey(float viewDepth) {
    const float depth = viewDepth > 0.0f ? viewDepth : 0.0f;
    // Non-negative floats order like their bit patterns; inverting them puts
    // the farthest particle first.
    return std::numeric_limits<uint32_t>::max() - std::bit_cast<uint32_t>(depth);
}

uint32_t sortElementCount(uint32_t liveParticles) {
    return std::max(sortBlockElements, std::bit_ceil(std::max(liveParticles, 1u)));
}

std::vector<SortPass> bitonicSortPlan(uint32_t elementCount) {
    std::vector<SortPass> passes{{.step = SortStep::LocalSort, .mergeSize = sortBlockElements, .distance = 0}};
    for (uint32_t mergeSize = sortBlockElements * 2; mergeSize <= elementCount; mergeSize *= 2) {
        for (uint32_t distance = mergeSize / 2; distance >= sortBlockElements; distance /= 2) {
            passes.push_back({.step = SortStep::GlobalMerge, .mergeSize = mergeSize, .distance = distance});
        }
        passes.push_back({.step = SortStep::LocalMerge, .mergeSize = mergeSize, .distance = sortBlockElements / 2});
    }
    return passes;
}

uint32_t sortDispatchGroups(uint32_t elementCount) {
    return elementCount / sortBlockElements;
}

void sortReference(std::span<SortKey> keys) {
    const auto elementCount = static_cast<uint32_t>(keys.size());
    for (const SortPass& pass : bitonicSortPlan(elementCount)) {
        for (uint32_t group = 0; group < sortDispatchGroups(elementCount); ++group) {
            const uint32_t blockStart = group * sortBlockElements;
            switch (pass.step) {
            case SortStep::LocalSort:
                for (uint32_t mergeSize = 2; mergeSize <= sortBlockElements; mergeSize *= 2) {
                    mergeInsideBlock(keys, blockStart, mergeSize, mergeSize / 2);
                }
                break;
            case SortStep::GlobalMerge:
                for (uint32_t local = 0; local < sortWorkgroupSize; ++local) {
                    const uint32_t first = pairIndex(group * sortWorkgroupSize + local, pass.distance);
                    compareExchange(keys, first, first + pass.distance, (first & pass.mergeSize) == 0);
                }
                break;
            case SortStep::LocalMerge:
                mergeInsideBlock(keys, blockStart, pass.mergeSize, pass.distance);
                break;
            }
        }
    }
}

uint32_t emitterSeed(uint64_t sceneEntityId) {
    // SplitMix64 finalizer folded to 32 bits.
    uint64_t value = sceneEntityId + 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<uint32_t>(value ^ (value >> 32));
}

void ParticleScheduler::reset() {
    m_states.clear();
    m_budget = 0;
    m_sceneGeneration = 0;
}

ParticleFramePlan ParticleScheduler::plan(std::span<const EmitterSnapshot> snapshots, uint32_t budget,
                                          float deltaSeconds, uint64_t sceneGeneration) {
    // Layout: which snapshots receive slots, and how many.
    std::vector<const EmitterSnapshot*> scheduled;
    std::vector<uint32_t> capacities;
    uint32_t remaining = budget;
    for (const EmitterSnapshot& snapshot : snapshots) {
        if (scheduled.size() == maximumEmitters || remaining == 0) {
            break;
        }
        const uint32_t capacity = std::min(snapshot.emitter.maxParticles, remaining);
        if (capacity == 0) {
            continue;
        }
        remaining -= capacity;
        scheduled.push_back(&snapshot);
        capacities.push_back(capacity);
    }
    ParticleFramePlan result;
    // Live particles store positions in their emitter's simulation space, so
    // a space change cannot keep them.
    bool layoutChanged =
        budget != m_budget || sceneGeneration != m_sceneGeneration || scheduled.size() != m_states.size();
    for (size_t index = 0; !layoutChanged && index < scheduled.size(); ++index) {
        layoutChanged = m_states[index].id != scheduled[index]->id || m_states[index].capacity != capacities[index] ||
                        m_states[index].space != scheduled[index]->emitter.space;
    }
    if (layoutChanged) {
        m_states.assign(scheduled.size(), {});
        for (size_t index = 0; index < scheduled.size(); ++index) {
            m_states[index].id = scheduled[index]->id;
            m_states[index].capacity = capacities[index];
            m_states[index].space = scheduled[index]->emitter.space;
            m_states[index].restartGeneration = scheduled[index]->emitter.restartGeneration;
        }
        m_budget = budget;
        m_sceneGeneration = sceneGeneration;
        result.resetPool = true;
    }

    const double step = clampedDeltaSeconds(deltaSeconds);
    for (size_t index = 0; index < scheduled.size(); ++index) {
        const ParticleEmitterComponent& emitter = scheduled[index]->emitter;
        EmitterState& state = m_states[index];
        if (state.restartGeneration != emitter.restartGeneration) {
            state.restartGeneration = emitter.restartGeneration;
            state.started = false;
            state.burstTimer = 0.0;
            state.spawnAccumulator = 0.0;
        }
        uint64_t spawns = 0;
        if (emitter.emitting) {
            if (!state.started) {
                spawns += emitter.burstCount;
                state.started = true;
            } else if (emitter.burstInterval > 0.0f) {
                state.burstTimer += step;
                const double bursts = std::floor(state.burstTimer / emitter.burstInterval);
                state.burstTimer -= bursts * emitter.burstInterval;
                spawns += static_cast<uint64_t>(bursts) * emitter.burstCount;
            }
            state.spawnAccumulator += static_cast<double>(emitter.spawnRate) * step;
            const double whole = std::floor(state.spawnAccumulator);
            state.spawnAccumulator -= whole;
            spawns += static_cast<uint64_t>(whole);
        }
        // More than a full ring in one frame would only overwrite itself.
        const auto spawnCount = static_cast<uint32_t>(std::min<uint64_t>(spawns, state.capacity));
        const ScheduledEmitter frame{
            .id = state.id,
            .firstSlot = result.poolSlots,
            .capacity = state.capacity,
            .ringStart = state.cursor,
            .spawnCount = spawnCount,
            .spawnOffset = result.totalSpawns,
            .firstOrdinal = static_cast<uint32_t>(state.spawned),
            .liveBound = static_cast<uint32_t>(std::min<uint64_t>(state.capacity, state.spawned + spawnCount))};
        state.cursor = static_cast<uint32_t>((static_cast<uint64_t>(state.cursor) + spawnCount) % state.capacity);
        state.spawned += spawnCount;
        result.poolSlots += state.capacity;
        result.totalSpawns += spawnCount;
        result.liveBound += frame.liveBound;
        result.emitters.push_back(frame);
    }
    return result;
}

std::vector<EmitterSnapshot> gatherEmitters(const Scene& scene, SceneVisibilityMode visibilityMode) {
    std::vector<EmitterSnapshot> snapshots;
    const SceneVisibility visibility(scene, visibilityMode);
    const auto view = scene.registry().view<const ParticleEmitterComponent, const TransformComponent>();
    for (Entity entity : view) {
        const SceneEntityId id = scene.id(entity);
        const auto& emitter = view.get<const ParticleEmitterComponent>(entity);
        if (!id || !validateParticleEmitter(emitter).empty() || !visibility.drawn(entity)) {
            continue;
        }
        snapshots.push_back(
            {.id = id.value, .emitter = emitter, .world = view.get<const TransformComponent>(entity).world});
    }
    std::ranges::sort(snapshots, {}, &EmitterSnapshot::id);
    return snapshots;
}

GpuParticleEmitter packEmitter(const EmitterSnapshot& snapshot, const ScheduledEmitter& scheduled,
                               uint32_t textureSlot) {
    const ParticleEmitterComponent& emitter = snapshot.emitter;
    const bool local = emitter.space == ParticleSimulationSpace::Local;
    glm::vec3 gravity = emitter.gravity;
    if (local) {
        // Local particles integrate in entity space, so world gravity is
        // brought into it (including the entity's scale).
        const glm::mat3 linear(snapshot.world);
        if (std::abs(glm::determinant(linear)) > 1e-12f) {
            gravity = glm::inverse(linear) * gravity;
        }
    }
    const float directionLength = glm::length(emitter.direction);
    const glm::vec3 direction =
        directionLength > 0.0f ? emitter.direction / directionLength : glm::vec3(0.0f, 1.0f, 0.0f);
    const uint32_t flags = (emitter.blendMode == ParticleBlendMode::Additive ? flagAdditive : 0u) |
                           (emitter.lit ? flagLit : 0u) | (local ? flagLocalSpace : 0u) |
                           (std::min(textureSlot, maximumSpriteTextures - 1) << textureSlotShift);
    return {.localToWorld = snapshot.world,
            .gravityDrag = {gravity, std::max(emitter.drag, 0.0f)},
            .direction = {direction, std::cos(std::clamp(emitter.coneAngleRadians, 0.0f, 3.14159265f))},
            .speedLifetime = {emitter.speedMin, emitter.speedMax, emitter.lifetimeMin, emitter.lifetimeMax},
            .sizeSpin = {emitter.startSize, emitter.endSize, emitter.angularSpeed, emitter.spawnRadius},
            .startColor = emitter.startColor,
            .endColor = emitter.endColor,
            .shading = {emitter.emissiveIntensity, emitter.softDistance, 0.0f, 0.0f},
            .pool = {scheduled.firstSlot, scheduled.capacity, scheduled.ringStart, scheduled.spawnCount},
            .spawn = {scheduled.spawnOffset, scheduled.firstOrdinal, emitterSeed(scheduled.id), flags}};
}

} // namespace particles
