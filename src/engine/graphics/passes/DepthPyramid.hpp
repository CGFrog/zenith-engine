#pragma once

#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <vector>

// Hierarchical depth of the scene view, rebuilt from the scene depth image.
//
// Contract for readers (occlusion culling, screen-space effects):
// - One R32G32_SFLOAT image with a full mip chain for the storage extent,
//   always in VK_IMAGE_LAYOUT_GENERAL once prepareFirstUse() ran.
// - Depth is reversed Z (near = 1, far = 0). Channel r holds the farthest
//   depth a texel covers (the minimum), channel g the closest (the maximum).
// - Level 0 is a copy of the scene depth over renderExtent(); see
//   DepthPyramidLayout.hpp for the extent of every level and the texel a base
//   pixel falls in. Only levelCount() levels are valid after a build.
// - Sample with sampler() (nearest, clamp) and texelFetch. Normalized UVs must
//   be scaled by levelExtent / storage size of the sampled level.
// - build() leaves every level readable by compute and fragment shaders; the
//   next build waits for those reads.
class DepthPyramid {
  public:
    static constexpr VkFormat format = VK_FORMAT_R32G32_SFLOAT;

    bool init(const VulkanContext& context, VkExtent2D storageExtent, VkImageView depthView);
    // Recreates storage; the device must be idle.
    bool resize(VkExtent2D storageExtent, VkImageView depthView);
    // Moves a new image into GENERAL so descriptors that statically reference
    // it are valid before the first build.
    void prepareFirstUse(VkCommandBuffer commandBuffer);
    // Rebuilds the levels covering renderExtent. The scene depth must be in
    // DEPTH_READ_ONLY_OPTIMAL with its attachment writes visible to compute.
    void build(VkCommandBuffer commandBuffer, VkExtent2D renderExtent);
    void shutdown();

    VkImageView view() const {
        return m_view;
    }

    VkSampler sampler() const {
        return m_sampler;
    }

    // Levels valid after the latest build.
    uint32_t levelCount() const {
        return m_builtLevels;
    }

    // Region of level 0 the latest build covered.
    VkExtent2D renderExtent() const {
        return m_renderExtent;
    }

    VkExtent2D storageExtent() const {
        return m_storageExtent;
    }

    uint32_t storageLevels() const {
        return static_cast<uint32_t>(m_levelViews.size());
    }

  private:
    bool createImage(VkExtent2D storageExtent, VkImageView depthView);
    void destroyImage();
    void levelBarrier(VkCommandBuffer commandBuffer, uint32_t level) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    VkImage m_image{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkImageView m_view{VK_NULL_HANDLE};
    std::vector<VkImageView> m_levelViews;
    std::vector<VkDescriptorSet> m_levelDescriptors;
    VkExtent2D m_storageExtent{};
    VkExtent2D m_renderExtent{};
    uint32_t m_builtLevels{0};
    bool m_layoutInitialized{false};
};
