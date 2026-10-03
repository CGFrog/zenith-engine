#include "ReflectionProbes.hpp"

#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// Smallest magnitude a ray direction component keeps, so the slab distances of
// boxProjectReflection stay finite; mirrors mesh.frag.
constexpr float minimumDirectionComponent = 1.0e-6f;
} // namespace

std::vector<ReflectionProbeVolume> gatherReflectionProbes(const Scene& scene, SceneVisibilityMode visibilityMode) {
    std::vector<ReflectionProbeVolume> result;
    const SceneVisibility visibility(scene, visibilityMode);
    const auto probes = scene.registry().view<const ReflectionProbeComponent, const TransformComponent>();
    for (Entity entity : probes) {
        if (!visibility.drawn(entity)) {
            continue;
        }
        const auto& probe = probes.get<const ReflectionProbeComponent>(entity);
        result.push_back({.entity = entity,
                          .center = glm::vec3(probes.get<const TransformComponent>(entity).world[3]),
                          .halfExtents = probe.halfExtents,
                          .blendDistance = probe.blendDistance,
                          .priority = probe.priority});
    }
    std::ranges::sort(result, [](const ReflectionProbeVolume& left, const ReflectionProbeVolume& right) {
        return entt::to_integral(left.entity) < entt::to_integral(right.entity);
    });
    return result;
}

float reflectionProbeDistance(const ReflectionProbeVolume& probe, const glm::vec3& point) {
    const glm::vec3 outside = glm::max(glm::abs(point - probe.center) - probe.halfExtents, glm::vec3(0.0f));
    return glm::length(outside);
}

bool reflectionProbeReaches(const ReflectionProbeVolume& probe, const glm::vec3& point) {
    return reflectionProbeDistance(probe, point) <= probe.blendDistance;
}

float reflectionProbeInfluence(const ReflectionProbeVolume& probe, const glm::vec3& point) {
    const float distance = reflectionProbeDistance(probe, point);
    if (distance > probe.blendDistance) {
        return 0.0f;
    }
    return probe.blendDistance <= 0.0f ? 1.0f : 1.0f - distance / probe.blendDistance;
}

std::vector<ReflectionProbeWeight> blendReflectionProbeVolumes(std::span<const ReflectionProbeVolume> probes,
                                                               const glm::vec3& point, size_t maximumProbes) {
    std::vector<ReflectionProbeWeight> candidates;
    for (uint32_t index = 0; index < probes.size(); ++index) {
        if (reflectionProbeReaches(probes[index], point)) {
            candidates.push_back({.probe = index, .influence = reflectionProbeInfluence(probes[index], point)});
        }
    }
    std::ranges::sort(candidates, [&](const ReflectionProbeWeight& left, const ReflectionProbeWeight& right) {
        const int32_t leftPriority = probes[left.probe].priority;
        const int32_t rightPriority = probes[right.probe].priority;
        if (leftPriority != rightPriority) {
            return leftPriority > rightPriority;
        }
        if (left.influence != right.influence) {
            return left.influence > right.influence;
        }
        return left.probe < right.probe;
    });
    candidates.resize(std::min(maximumProbes, candidates.size()));
    float total = 0.0f;
    for (const ReflectionProbeWeight& candidate : candidates) {
        total += candidate.influence;
    }
    for (ReflectionProbeWeight& candidate : candidates) {
        candidate.weight = total > 0.0f ? candidate.influence / total : 0.0f;
    }
    return candidates;
}

ReflectionProbeShading shadeReflectionProbes(std::span<const ReflectionProbeVolume> probes, const glm::vec3& point) {
    ReflectionProbeShading shading{.probes = blendReflectionProbeVolumes(probes, point)};
    float coverage = 0.0f;
    for (const ReflectionProbeWeight& probe : shading.probes) {
        coverage = std::max(coverage, probe.influence);
    }
    for (ReflectionProbeWeight& probe : shading.probes) {
        probe.weight *= coverage;
    }
    shading.environmentWeight = 1.0f - coverage;
    return shading;
}

glm::vec3 boxProjectReflection(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& center,
                               const glm::vec3& halfExtents) {
    const glm::vec3 minimum = center - halfExtents;
    const glm::vec3 maximum = center + halfExtents;
    const glm::vec3 origin = glm::clamp(position, minimum, maximum);
    glm::vec3 safeDirection = direction;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(safeDirection[axis]) < minimumDirectionComponent) {
            safeDirection[axis] = safeDirection[axis] < 0.0f ? -minimumDirectionComponent : minimumDirectionComponent;
        }
    }
    // From inside the box, the exit distance is the nearest of the per-axis
    // far-slab distances, all of which are non-negative.
    const glm::vec3 toMaximum = (maximum - origin) / safeDirection;
    const glm::vec3 toMinimum = (minimum - origin) / safeDirection;
    const glm::vec3 farSlab = glm::max(toMaximum, toMinimum);
    const float exitDistance = std::min({farSlab.x, farSlab.y, farSlab.z});
    const glm::vec3 projected = origin + direction * exitDistance - center;
    const float length = glm::length(projected);
    return length > 0.0f ? projected / length : direction;
}

std::vector<uint32_t> selectResidentReflectionProbes(std::span<const ReflectionProbeVolume> probes,
                                                     const glm::vec3& cameraPosition, uint32_t maximumProbes) {
    std::vector<uint32_t> order(probes.size());
    for (uint32_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    const auto reach = [&](uint32_t index) {
        return std::max(reflectionProbeDistance(probes[index], cameraPosition) - probes[index].blendDistance, 0.0f);
    };
    std::ranges::sort(order, [&](uint32_t left, uint32_t right) {
        const float leftReach = reach(left);
        const float rightReach = reach(right);
        if (leftReach != rightReach) {
            return leftReach < rightReach;
        }
        if (probes[left].priority != probes[right].priority) {
            return probes[left].priority > probes[right].priority;
        }
        return left < right;
    });
    order.resize(std::min<size_t>(order.size(), maximumProbes));
    std::ranges::sort(order);
    return order;
}

ClusterBounds reflectionProbeViewBounds(const ReflectionProbeVolume& probe, const glm::mat4& view) {
    const glm::vec3 center = glm::vec3(view * glm::vec4(probe.center, 1.0f));
    const glm::vec3 extents = glm::max(probe.halfExtents + glm::vec3(std::max(probe.blendDistance, 0.0f)), 0.0f);
    const glm::mat3 rotation(view);
    // Each view axis spans the absolute projection of the world extents.
    glm::vec3 viewExtents{0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        viewExtents[axis] = std::abs(rotation[0][axis]) * extents.x + std::abs(rotation[1][axis]) * extents.y +
                            std::abs(rotation[2][axis]) * extents.z;
    }
    return {.minimum = center - viewExtents, .maximum = center + viewExtents};
}

bool reflectionProbeAffectsCluster(const ClusterBounds& probeViewBounds, const ClusterBounds& cluster) {
    return glm::all(glm::lessThanEqual(probeViewBounds.minimum, cluster.maximum)) &&
           glm::all(glm::lessThanEqual(cluster.minimum, probeViewBounds.maximum));
}

std::vector<uint32_t> assignReflectionProbesToClusters(const ClusterGrid& grid,
                                                       std::span<const ClusterBounds> probeViewBounds) {
    std::vector<uint32_t> masks(grid.clusterCount(), 0u);
    const ClusterGridConfiguration& configuration = grid.configuration;
    const size_t probeCount = std::min<size_t>(probeViewBounds.size(), 32);
    for (uint32_t slice = 0; slice < configuration.depthSlices; ++slice) {
        for (uint32_t row = 0; row < configuration.rows; ++row) {
            for (uint32_t column = 0; column < configuration.columns; ++column) {
                const ClusterBounds bounds = grid.bounds(column, row, slice);
                uint32_t mask = 0;
                for (uint32_t probe = 0; probe < probeCount; ++probe) {
                    if (reflectionProbeAffectsCluster(probeViewBounds[probe], bounds)) {
                        mask |= 1u << probe;
                    }
                }
                masks[grid.clusterIndex(column, row, slice)] = mask;
            }
        }
    }
    return masks;
}
