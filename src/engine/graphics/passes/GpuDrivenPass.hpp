#pragma once

// GPU visibility culling and indirect draw compaction (gpu_visibility.comp, compact_draws.comp).

#include "../GpuCullLayout.hpp"
#include "../MeshLodSelection.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "DepthPyramid.hpp"

#include <glm/glm.hpp>
#include <span>
#include <vector>

// Mirrors ObjectRecord in shaders/gpu_visibility.comp (std430).
struct GpuDrivenObjectRecord {
    glm::mat4 model{1.0f};
    // Last frame's transform; per-object motion vectors require it.
    glm::mat4 previousModel{1.0f};
    glm::vec4 bounds{0.0f};
    // x: LOD 0 batch index, y: persistent visibility slot (stable across
    // frames), z: LOD level count (batches chained via nextLodBatch).
    glm::uvec4 draw{0u};
    // x: skin palette offset, y: joint count, z: material record index.
    glm::uvec4 skin{0u};
};

// Mirrors BatchRecord in shaders/gpu_visibility.comp and compact_draws.comp.
struct GpuDrivenBatch {
    uint32_t indexCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
    uint32_t group{0};
    // Compacted commands for the batch's group start at this batch slot.
    uint32_t groupFirstBatch{0};
    // Object-space geometric error of this batch's LOD level.
    float lodGeometricError{0.0f};
    // Batch of the next coarser level of the same primitive, when one exists.
    uint32_t nextLodBatch{0};
};

// GPU frustum and two-phase occlusion culling that writes indirect draws.
// Per frame: prepare -> beginFrame -> cull(Early) -> draw early groups ->
// build the depth pyramid -> cull(Late) -> draw late groups. Both phases keep
// their compacted draws until the next beginFrame, so later passes may redraw
// them without culling again. The caller owns render passes, the depth pyramid,
// and the transitions of the scene depth image around the pyramid build.
//
// Frustum-only views (GpuDrivenLimits::frustumViews, e.g. shadow cascades) may
// be culled any time between beginFrame and finishFrame with cullFrustumView;
// each writes its own output region (GpuCullLayout.hpp), so it never disturbs
// the camera phases or the visibility history. Every cull binds the depth
// pyramid statically, so it must already be in its sampled layout.
class GpuDrivenPass {
  public:
    bool init(const VulkanContext& context, uint32_t frameCount, const DepthPyramid& depthPyramid,
              const GpuDrivenLimits& limits, bool indirectCount);
    // Points the late cull at the pyramid's current storage; call after the
    // pyramid is recreated, while the device is idle.
    void bindDepthPyramid(const DepthPyramid& depthPyramid);
    // Uploads this frame's records and batch templates (host writes).
    bool prepare(uint32_t frameIndex, std::span<const GpuDrivenObjectRecord> objects,
                 std::span<const GpuDrivenBatch> batches, uint32_t referenceVisibleCount);
    // Resets GPU counters and orders them after the previous frame's use.
    void beginFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Each visible object is emitted into the batch of the LOD level the view
    // selects; see MeshLodSelection.hpp. The late phase tests against the
    // pyramid's latest build, which must cover this frame's early draws.
    void cull(VkCommandBuffer commandBuffer, uint32_t frameIndex, GpuCullPhase phase, const glm::mat4& viewProjection,
              const DepthPyramid& depthPyramid, const MeshLodView& lodView);
    // Frustum-tests every object against viewProjection (perspective or
    // orthographic, zero-to-one depth) into the region of frustum view `view`.
    // LOD selection uses lodView, so a shadow view can match the camera's LODs.
    void cullFrustumView(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t view,
                         const glm::mat4& viewProjection, const MeshLodView& lodView);
    // Issues one multi-draw for a contiguous group of batches of a region
    // culled this frame. Draws index instanceBuffer() by gl_InstanceIndex.
    void drawGroup(VkCommandBuffer commandBuffer, GpuCullRegion region, uint32_t group, uint32_t firstBatch,
                   uint32_t batchCount) const;
    // Copies this frame's statistics for readback once the frame retires.
    void finishFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex);

    VkBuffer instanceBuffer() const {
        return m_visibleObjects.buffer;
    }

    uint32_t frustumViewCount() const {
        return m_layout.limits.frustumViews;
    }

    uint32_t visibilityMismatchCount() const {
        return m_visibilityMismatchCount;
    }

    // Camera instances the GPU emitted in the most recently retired frame.
    uint32_t retiredVisibleCount() const {
        return m_retiredVisibleCount;
    }

    // Instances all frustum views emitted in the most recently retired frame;
    // views not culled that frame contribute nothing.
    uint32_t retiredFrustumViewVisibleCount() const {
        return m_retiredFrustumViewVisibleCount;
    }

    // Instances the camera's late phase emitted this frame, counted on the GPU
    // once cull(Late) ran. Zero means the late phase drew nothing, so the
    // scene depth is the early phase's.
    DepthPyramid::DepthChangeCount lateEmittedCount() const {
        return {.buffer = m_statistics.buffer, .index = GpuCullRegion::phase(GpuCullPhase::Late).index};
    }

    uint32_t pipelineCount() const {
        return 2;
    }

    void shutdown(const VulkanContext& context);

  private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
    };

    struct FrameData {
        Buffer objects;
        Buffer batches;
        Buffer statistics;
        VkDescriptorSet cullDescriptor{VK_NULL_HANDLE};
        VkDescriptorSet compactDescriptor{VK_NULL_HANDLE};
        uint32_t objectCount{0};
        uint32_t batchCount{0};
        uint32_t referenceVisibleCount{0};
        bool statisticsPending{false};
    };

    bool createLayouts();
    bool createPool(uint32_t frameCount);
    bool createBuffers(uint32_t frameCount);
    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output);
    bool createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output);
    void destroyBuffer(Buffer& buffer);
    // Runs the visibility and compaction kernels for one output region.
    void dispatchCull(VkCommandBuffer commandBuffer, uint32_t frameIndex, GpuCullRegion region, GpuCullMode mode,
                      const glm::mat4& viewProjection, const MeshLodView& lodView, uint32_t pyramidLevels,
                      VkExtent2D pyramidExtent);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_cullLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_compactLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::vector<FrameData> m_frames;
    // Device-local state written and read only by the GPU. Visible objects,
    // instance counts, draw commands, draw counts and statistics are
    // region-major (m_layout); the visibility history belongs to the camera.
    Buffer m_visibleObjects;
    Buffer m_instanceCounts;
    Buffer m_drawCommands;
    Buffer m_drawCounts;
    Buffer m_visibility;
    Buffer m_statistics;
    ComputePipeline m_cullPipeline;
    ComputePipeline m_compactPipeline;
    GpuCullRegionLayout m_layout;
    bool m_indirectCount{false};
    bool m_visibilityInitialized{false};
    uint32_t m_visibilityMismatchCount{0};
    uint32_t m_retiredVisibleCount{0};
    uint32_t m_retiredFrustumViewVisibleCount{0};
};
