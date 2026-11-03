#pragma once

// Reflection probes are world-space axis-aligned boxes centered on their entity's translation (rotation and scale are
// ignored).

#include "engine/graphics/Lighting.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Reflection probes are world-space axis-aligned boxes centered on their
// entity's translation (rotation and scale are ignored). A probe fully
// influences points inside its box and fades linearly to zero across
// blendDistance outside it. Per-pixel blending (mesh.frag) mirrors the
// functions below; reflection_probe_clusters.comp mirrors the cluster test.

// Probes resident in the probe cube array at once; mirrors
// MAXIMUM_RESIDENT_REFLECTION_PROBES in shaders/include/reflection_probe.glsl.
// Cluster masks hold one bit per probe.
inline constexpr uint32_t maximumResidentReflectionProbes = 8;
// Probes blended at one point; mirrors MAXIMUM_BLENDED_REFLECTION_PROBES.
inline constexpr size_t maximumBlendedReflectionProbes = 4;

struct ReflectionProbeVolume {
    Entity entity{entt::null};
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.0f};
    float blendDistance{0.0f};
    int32_t priority{0};
};

// Every probe in the scene, in ascending entity order.
// Only probes the visibility mode draws (see SceneVisibility.hpp).
std::vector<ReflectionProbeVolume>
gatherReflectionProbes(const Scene& scene, SceneVisibilityMode visibility = SceneVisibilityMode::Runtime);

// Distance from point to the probe's box; zero inside it.
float reflectionProbeDistance(const ReflectionProbeVolume& probe, const glm::vec3& point);
// True when point lies within the box expanded by the blend distance.
bool reflectionProbeReaches(const ReflectionProbeVolume& probe, const glm::vec3& point);
// 1 inside the box, falling linearly to 0 at the blend distance; 0 beyond it.
float reflectionProbeInfluence(const ReflectionProbeVolume& probe, const glm::vec3& point);

struct ReflectionProbeWeight {
    // Index into the probe span the weight was computed from.
    uint32_t probe{0};
    // Share among the returned probes; the weights sum to one unless every
    // influence is zero, when all are zero.
    float weight{0.0f};
    float influence{0.0f};
};

// The probes reaching point, ordered by descending priority, then descending
// influence, then ascending span index; at most maximumProbes are kept and
// their influences normalized. Span order is the final tie-break, so callers
// pass probes in entity order to match blendReflectionProbes.
std::vector<ReflectionProbeWeight> blendReflectionProbeVolumes(std::span<const ReflectionProbeVolume> probes,
                                                               const glm::vec3& point,
                                                               size_t maximumProbes = maximumBlendedReflectionProbes);

struct ReflectionProbeShading {
    // Normalized blend weights scaled by coverage.
    std::vector<ReflectionProbeWeight> probes;
    // Share of the scene environment: 1 - coverage, where coverage is the
    // largest influence among the blended probes.
    float environmentWeight{1.0f};
};

// Per-pixel blend of mesh.frag: inside any probe's box the probes cover the
// point fully; across the blend distance the scene environment fades in. The
// probe and environment weights sum to one.
ReflectionProbeShading shadeReflectionProbes(std::span<const ReflectionProbeVolume> probes, const glm::vec3& point);

// Box-projected (parallax-corrected) lookup direction for a probe captured at
// center: the ray from position along direction is intersected with the box
// from inside, and the direction from center to the hit is returned
// normalized. Positions outside the box (within the blend distance) are first
// clamped onto it. direction must be normalized.
glm::vec3 boxProjectReflection(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& center,
                               const glm::vec3& halfExtents);

// Up to maximumProbes indices into probes, preferring probes whose influence
// volume is nearest the camera (zero inside it), then higher priority, then
// span order. The result is in ascending index order.
std::vector<uint32_t> selectResidentReflectionProbes(std::span<const ReflectionProbeVolume> probes,
                                                     const glm::vec3& cameraPosition, uint32_t maximumProbes);

// Conservative view-space box of the probe's influence volume (its box
// expanded by the blend distance) for a rigid view transform.
ClusterBounds reflectionProbeViewBounds(const ReflectionProbeVolume& probe, const glm::mat4& view);
// Inclusive box overlap test; mirrors reflection_probe_clusters.comp.
bool reflectionProbeAffectsCluster(const ClusterBounds& probeViewBounds, const ClusterBounds& cluster);

// CPU reference of reflection_probe_clusters.comp: one mask per cluster with
// bit i set when probe i's view bounds touch the cluster. At most 32 probes.
std::vector<uint32_t> assignReflectionProbesToClusters(const ClusterGrid& grid,
                                                       std::span<const ClusterBounds> probeViewBounds);
