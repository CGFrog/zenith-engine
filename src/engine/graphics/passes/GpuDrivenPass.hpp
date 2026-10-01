#pragma once

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
    glm::uvec4 draw{0u};
    glm::uvec4 skin{0u};
};

struct GpuDrivenBatch {
    uint32_t indexCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

class GpuDrivenPass {
  public:
    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D extent, VkImageView depthView,
              uint32_t maximumObjects, uint32_t maximumBatches);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    bool prepare(uint32_t frameIndex, std::span<const GpuDrivenObjectRecord> objects,
                 std::span<const GpuDrivenBatch> batches, uint32_t referenceVisibleCount);
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkBuffer visibleObjectBuffer,
                VkDeviceSize visibleObjectOffset, const glm::mat4& viewProjection, bool depthHistoryValid,
                bool bypassOcclusion, VkQueryPool timestampQueries);
    VkBuffer indirectBuffer(uint32_t frameIndex) const;
    uint32_t batchCount(uint32_t frameIndex) const;
    uint32_t mipLevels() const;

    uint32_t visibilityMismatchCount() const {
        return m_visibilityMismatchCount;
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
        Buffer commands;
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        uint32_t objectCount{0};
        uint32_t batchCount{0};
        uint32_t referenceVisibleCount{0};
        bool hasRetiredSubmission{false};
    };

    bool createLayouts();
    bool createPool(uint32_t frameCount);
    bool createFrameBuffers(uint32_t frameCount);
    bool createDepthResources(VkExtent2D extent, VkImageView depthView);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output);
    void destroyDepthResources();
    void destroyBuffer(Buffer& buffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_depthLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_visibilityLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkImage m_pyramid{VK_NULL_HANDLE};
    VmaAllocation m_pyramidAllocation{VK_NULL_HANDLE};
    VkImageView m_pyramidView{VK_NULL_HANDLE};
    std::vector<VkImageView> m_mipViews;
    std::vector<VkDescriptorSet> m_depthDescriptors;
    std::vector<FrameData> m_frames;
    ComputePipeline m_depthPipeline;
    ComputePipeline m_visibilityPipeline;
    VkExtent2D m_extent{};
    uint32_t m_maximumObjects{0};
    uint32_t m_maximumBatches{0};
    bool m_pyramidInitialized{false};
    uint32_t m_visibilityMismatchCount{0};
};
