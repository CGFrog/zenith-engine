#pragma once

// Buffer layout and limits of GPU-driven culling.

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

// CPU-side contracts of GPU-driven culling (GpuDrivenPass and
// shaders/gpu_visibility.comp) that do not need a device: the layout of the
// per-view output regions and the clip-volume sphere test.

enum class GpuCullPhase : uint32_t {
    // Objects visible last frame; drawn before the depth pyramid exists.
    Early = 0,
    // Everything else, tested against this frame's depth pyramid.
    Late = 1,
};

struct GpuDrivenLimits {
    uint32_t maximumObjects{0};
    // Instance slots per output region. Batches reserve a slot for every object
    // that may select them, so LOD chains need more slots than objects.
    uint32_t maximumInstances{0};
    uint32_t maximumBatches{0};
    uint32_t maximumGroups{0};
    // Frustum-only views (for example shadow cascades) culled alongside the
    // camera, each with its own output region.
    uint32_t frustumViews{0};
};

// How gpu_visibility.comp tests objects for one cull. Mirrors the mode
// constants in that shader.
enum class GpuCullMode : uint32_t {
    // Camera early phase: frustum test, emit what was visible last frame.
    EarlyOcclusion = 0,
    // Camera late phase: frustum and depth-pyramid test, update visibility.
    LateOcclusion = 1,
    // Frustum test only; never reads the pyramid or the visibility history.
    FrustumOnly = 2,
};

// One output region of the cull buffers. Each region holds maximumInstances
// visible instances, maximumBatches instance counters and indirect commands,
// maximumGroups draw counts, and one emitted-instance statistic.
struct GpuCullRegion {
    uint32_t index{0};

    static constexpr GpuCullRegion phase(GpuCullPhase phase) {
        return {static_cast<uint32_t>(phase)};
    }

    // Frustum-only views follow the camera's two phases.
    static constexpr GpuCullRegion frustumView(uint32_t view) {
        return {cameraRegionCount + view};
    }

    static constexpr uint32_t cameraRegionCount = 2;

    auto operator<=>(const GpuCullRegion&) const = default;
};

// Element offsets and buffer capacities of the region-major cull buffers.
// Offsets are in elements of each buffer, not bytes.
struct GpuCullRegionLayout {
    GpuDrivenLimits limits;

    uint32_t regionCount() const {
        return GpuCullRegion::cameraRegionCount + limits.frustumViews;
    }

    bool contains(GpuCullRegion region) const {
        return region.index < regionCount();
    }

    // First visible-instance slot; also the firstInstance base of the region's
    // draws, since vertex shaders index the instance buffer by gl_InstanceIndex.
    uint64_t firstInstance(GpuCullRegion region) const {
        return static_cast<uint64_t>(region.index) * limits.maximumInstances;
    }

    // First instance counter and indirect command of the region.
    uint64_t firstBatch(GpuCullRegion region) const {
        return static_cast<uint64_t>(region.index) * limits.maximumBatches;
    }

    // First per-group draw count of the region.
    uint64_t firstGroup(GpuCullRegion region) const {
        return static_cast<uint64_t>(region.index) * limits.maximumGroups;
    }

    uint64_t instanceCapacity() const {
        return static_cast<uint64_t>(regionCount()) * limits.maximumInstances;
    }

    uint64_t batchCapacity() const {
        return static_cast<uint64_t>(regionCount()) * limits.maximumBatches;
    }

    uint64_t groupCapacity() const {
        return static_cast<uint64_t>(regionCount()) * limits.maximumGroups;
    }

    // Every instance index must fit the 32-bit firstInstance of a draw and the
    // 32-bit offsets the shaders compute.
    bool valid() const {
        constexpr uint64_t maximumIndex = UINT32_MAX;
        return limits.maximumInstances > 0 && limits.maximumBatches > 0 && limits.maximumGroups > 0 &&
               instanceCapacity() <= maximumIndex && batchCapacity() <= maximumIndex && groupCapacity() <= maximumIndex;
    }
};

// The six clip-volume planes of a zero-to-one depth projection, perspective or
// orthographic: -w <= x, y <= w and 0 <= z <= w, in that order (left, right,
// bottom, top, z = 0, z = w). A point p is inside plane a when
// dot(a.xyz, p) + a.w >= 0. Planes are not normalized.
std::array<glm::vec4, 6> clipVolumePlanes(const glm::mat4& viewProjection);

// True when any part of the sphere lies inside every plane. Mirrors
// frustumVisible in shaders/gpu_visibility.comp, which accepts unnormalized
// planes by scaling the radius with each plane normal's length.
bool sphereReachesClipVolume(const std::array<glm::vec4, 6>& planes, const glm::vec3& center, float radius);
