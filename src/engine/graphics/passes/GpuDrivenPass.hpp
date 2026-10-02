#pragma once

#include "../MeshLodSelection.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

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

enum class GpuCullPhase : uint32_t {
    // Objects visible last frame; drawn before the depth pyramid exists.
    Early = 0,
    // Everything else, tested against this frame's depth pyramid.
    Late = 1,
};

struct GpuDrivenLimits {
    uint32_t maximumObjects{0};
    // Instance slots per cull phase. Batches reserve a slot for every object
    // that may select them, so LOD chains need more slots than objects.
    uint32_t maximumInstances{0};
    uint32_t maximumBatches{0};
    uint32_t maximumGroups{0};
};

// GPU frustum and two-phase occlusion culling that writes indirect draws.
// Per frame: prepare -> beginFrame -> cull(Early) -> draw early groups ->
// buildDepthPyramid -> cull(Late) -> draw late groups. The caller owns render
// passes and the transitions of the scene depth image around the pyramid build.
class GpuDrivenPass {
  public:
    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D extent, VkImageView depthView,
              const GpuDrivenLimits& limits, bool indirectCount);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    // Uploads this frame's records and batch templates (host writes).
    bool prepare(uint32_t frameIndex, std::span<const GpuDrivenObjectRecord> objects,
                 std::span<const GpuDrivenBatch> batches, uint32_t referenceVisibleCount);
    // Resets GPU counters and orders them after the previous frame's use.
    void beginFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Each visible object is emitted into the batch of the LOD level the view
    // selects; see MeshLodSelection.hpp.
    void cull(VkCommandBuffer commandBuffer, uint32_t frameIndex, GpuCullPhase phase, const glm::mat4& viewProjection,
              VkExtent2D renderExtent, const MeshLodView& lodView);
    // Builds the min-depth pyramid; the scene depth must be in
    // DEPTH_READ_ONLY_OPTIMAL with its attachment writes made visible.
    void buildDepthPyramid(VkCommandBuffer commandBuffer);
    // Issues one multi-draw for a contiguous group of batches.
    void drawGroup(VkCommandBuffer commandBuffer, GpuCullPhase phase, uint32_t group, uint32_t firstBatch,
                   uint32_t batchCount) const;
    // Copies this frame's statistics for readback once the frame retires.
    void finishFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex);

    VkBuffer instanceBuffer() const {
        return m_visibleObjects.buffer;
    }

    uint32_t mipLevels() const;

    uint32_t visibilityMismatchCount() const {
        return m_visibilityMismatchCount;
    }

    // Instances the GPU emitted in the most recently retired frame.
    uint32_t retiredVisibleCount() const {
        return m_retiredVisibleCount;
    }

    uint32_t pipelineCount() const {
        return 3;
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
    bool createDepthResources(VkExtent2D extent, VkImageView depthView);
    void writeCullDescriptors();
    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output);
    bool createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output);
    void destroyDepthResources();
    void destroyBuffer(Buffer& buffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_depthLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_cullLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_compactLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkImage m_pyramid{VK_NULL_HANDLE};
    VmaAllocation m_pyramidAllocation{VK_NULL_HANDLE};
    VkImageView m_pyramidView{VK_NULL_HANDLE};
    std::vector<VkImageView> m_mipViews;
    std::vector<VkDescriptorSet> m_depthDescriptors;
    std::vector<FrameData> m_frames;
    // Device-local state written and read only by the GPU.
    Buffer m_visibleObjects;
    Buffer m_instanceCounts;
    Buffer m_drawCommands;
    Buffer m_drawCounts;
    Buffer m_visibility;
    Buffer m_statistics;
    ComputePipeline m_depthPipeline;
    ComputePipeline m_cullPipeline;
    ComputePipeline m_compactPipeline;
    VkExtent2D m_extent{};
    GpuDrivenLimits m_limits;
    bool m_indirectCount{false};
    bool m_pyramidInitialized{false};
    bool m_visibilityInitialized{false};
    uint32_t m_visibilityMismatchCount{0};
    uint32_t m_retiredVisibleCount{0};
};
