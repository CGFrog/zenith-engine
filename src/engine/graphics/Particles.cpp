#include "Particles.hpp"

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

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

namespace {
// Sorts ranges by first slot and merges overlapping or touching ones.
std::vector<SlotRange> mergedRanges(std::vector<SlotRange> ranges) {
    std::erase_if(ranges, [](const SlotRange& range) { return range.count == 0; });
    std::ranges::sort(ranges, {}, &SlotRange::first);
    std::vector<SlotRange> merged;
    for (const SlotRange& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().first + merged.back().count) {
            const uint32_t end = std::max(merged.back().first + merged.back().count, range.first + range.count);
            merged.back().count = end - merged.back().first;
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

// The gaps of [0, budget) that no range covers, in slot order.
std::vector<SlotRange> freeRanges(std::vector<SlotRange> used, uint32_t budget) {
    std::vector<SlotRange> gaps;
    uint32_t next = 0;
    for (const SlotRange& range : mergedRanges(std::move(used))) {
        if (range.first > next) {
            gaps.push_back({.first = next, .count = range.first - next});
        }
        next = std::max(next, range.first + range.count);
    }
    if (budget > next) {
        gaps.push_back({.first = next, .count = budget - next});
    }
    return gaps;
}

// Takes count slots from the first gap that holds them.
std::optional<uint32_t> allocateFirstFit(std::vector<SlotRange>& gaps, uint32_t count) {
    for (SlotRange& gap : gaps) {
        if (gap.count >= count) {
            const uint32_t first = gap.first;
            gap.first += count;
            gap.count -= count;
            return first;
        }
    }
    return std::nullopt;
}
} // namespace

bool ParticleScheduler::updateLayout(std::span<const Placement> placements, std::vector<SlotRange>& clearRanges) {
    std::vector<EmitterState> states;
    states.reserve(placements.size());
    std::vector<bool> kept(m_states.size(), false);
    std::vector<size_t> added;
    // Both sequences are in id order.
    size_t previous = 0;
    for (const Placement& placement : placements) {
        while (previous < m_states.size() && m_states[previous].id < placement.snapshot->id) {
            ++previous;
        }
        const bool unchanged = previous < m_states.size() && m_states[previous].id == placement.snapshot->id &&
                               m_states[previous].capacity == placement.capacity &&
                               m_states[previous].space == placement.snapshot->emitter.space;
        if (unchanged) {
            kept[previous] = true;
            states.push_back(m_states[previous]);
        } else {
            added.push_back(states.size());
            states.push_back({.id = placement.snapshot->id,
                              .capacity = placement.capacity,
                              .space = placement.snapshot->emitter.space,
                              .restartGeneration = placement.snapshot->emitter.restartGeneration});
        }
    }
    if (added.empty() && states.size() == m_states.size()) {
        return true;
    }

    std::vector<SlotRange> used;
    std::array<bool, maximumEmitters> tableUsed{};
    for (size_t index = 0; index < m_states.size(); ++index) {
        const SlotRange range{.first = m_states[index].firstSlot, .count = m_states[index].capacity};
        if (kept[index]) {
            used.push_back(range);
            tableUsed[m_states[index].emitterIndex] = true;
        } else {
            clearRanges.push_back(range);
        }
    }
    std::vector<SlotRange> gaps = freeRanges(std::move(used), m_budget);
    for (const size_t index : added) {
        EmitterState& state = states[index];
        const std::optional<uint32_t> first = allocateFirstFit(gaps, state.capacity);
        if (!first) {
            return false;
        }
        state.firstSlot = *first;
        clearRanges.push_back({.first = *first, .count = state.capacity});
        // At most maximumEmitters are scheduled, so a free entry exists.
        const auto entry = static_cast<uint32_t>(std::ranges::find(tableUsed, false) - tableUsed.begin());
        tableUsed[entry] = true;
        state.emitterIndex = entry;
    }
    m_states = std::move(states);
    return true;
}

void ParticleScheduler::rebuildLayout(std::span<const Placement> placements) {
    m_states.assign(placements.size(), {});
    uint32_t nextSlot = 0;
    for (size_t index = 0; index < placements.size(); ++index) {
        const EmitterSnapshot& snapshot = *placements[index].snapshot;
        m_states[index] = {.id = snapshot.id,
                           .firstSlot = nextSlot,
                           .capacity = placements[index].capacity,
                           .emitterIndex = static_cast<uint32_t>(index),
                           .space = snapshot.emitter.space,
                           .restartGeneration = snapshot.emitter.restartGeneration};
        nextSlot += placements[index].capacity;
    }
}

void ParticleScheduler::advance(EmitterState& state, const ParticleEmitterComponent& emitter, double step,
                                ParticleFramePlan& result) {
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
        .emitterIndex = state.emitterIndex,
        .firstSlot = state.firstSlot,
        .capacity = state.capacity,
        .ringStart = state.cursor,
        .spawnCount = spawnCount,
        .spawnOffset = result.totalSpawns,
        .firstOrdinal = static_cast<uint32_t>(state.spawned),
        .liveBound = static_cast<uint32_t>(std::min<uint64_t>(state.capacity, state.spawned + spawnCount))};
    state.cursor = static_cast<uint32_t>((static_cast<uint64_t>(state.cursor) + spawnCount) % state.capacity);
    state.spawned += spawnCount;
    result.poolSlots = std::max(result.poolSlots, state.firstSlot + state.capacity);
    result.emitterTableSize = std::max(result.emitterTableSize, state.emitterIndex + 1);
    result.totalSpawns += spawnCount;
    result.liveBound += frame.liveBound;
    result.emitters.push_back(frame);
}

ParticleFramePlan ParticleScheduler::plan(std::span<const EmitterSnapshot> snapshots, uint32_t budget,
                                          float deltaSeconds, uint64_t sceneGeneration) {
    // Capacities: which snapshots receive slots, and how many.
    std::vector<Placement> placements;
    uint32_t remaining = budget;
    uint32_t droppedEmitters = 0;
    for (const EmitterSnapshot& snapshot : snapshots) {
        if (remaining == 0) {
            break;
        }
        if (placements.size() == maximumEmitters) {
            droppedEmitters += snapshot.emitter.maxParticles > 0 ? 1u : 0u;
            continue;
        }
        const uint32_t capacity = std::min(snapshot.emitter.maxParticles, remaining);
        if (capacity == 0) {
            continue;
        }
        remaining -= capacity;
        placements.push_back({.snapshot = &snapshot, .capacity = capacity});
    }
    ParticleFramePlan result;
    result.droppedEmitters = droppedEmitters;
    // Starting from no emitters rebuilds too, so the pool forgets particles
    // whose emitters left while nothing was simulated.
    const bool rebuild = budget != m_budget || sceneGeneration != m_sceneGeneration || m_states.empty();
    std::vector<SlotRange> clearRanges;
    if (rebuild || !updateLayout(placements, clearRanges)) {
        rebuildLayout(placements);
        m_budget = budget;
        m_sceneGeneration = sceneGeneration;
        result.resetPool = true;
    } else {
        result.clearRanges = mergedRanges(std::move(clearRanges));
    }

    const double step = clampedDeltaSeconds(deltaSeconds);
    for (size_t index = 0; index < placements.size(); ++index) {
        advance(m_states[index], placements[index].snapshot->emitter, step, result);
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
