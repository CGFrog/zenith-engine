#pragma once

// Hierarchical depth of the scene view, rebuilt from the scene depth image.

#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>
#include <optional>
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
//
// Builds run as one dispatch (shaders/depth_pyramid_single_pass.comp) when
// the device supports dynamically indexed storage-image arrays, and as one
// dispatch per level otherwise; both produce identical levels.
class DepthPyramid {
  public:
    static constexpr VkFormat format = VK_FORMAT_R32G32_SFLOAT;

    // A uint32_t in a storage buffer, written on the GPU by compute shaders
    // (or transfers) before build() records, that is zero only when the scene
    // depth is unchanged since the previous build of the same render extent.
    struct DepthChangeCount {
        VkBuffer buffer{VK_NULL_HANDLE};
        uint32_t index{0};
    };

    bool init(const VulkanContext& context, VkExtent2D storageExtent, VkImageView depthView);
    // Recreates storage; the device must be idle.
    bool resize(VkExtent2D storageExtent, VkImageView depthView);
    // Moves a new image into GENERAL so descriptors that statically reference
    // it are valid before the first build.
    void prepareFirstUse(VkCommandBuffer commandBuffer);
    // Rebuilds the levels covering renderExtent. The scene depth must be in
    // DEPTH_READ_ONLY_OPTIMAL with its attachment writes visible to compute.
    // With depthChanges, the GPU skips the rebuild when the count is zero and
    // keeps the previous build's levels; the per-level path always rebuilds.
    void build(VkCommandBuffer commandBuffer, VkExtent2D renderExtent,
               std::optional<DepthChangeCount> depthChanges = std::nullopt);
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

    // True when builds use the single-dispatch downsampler.
    bool singlePass() const {
        return m_singlePass;
    }

  private:
    bool createSinglePassResources(const VulkanContext& context);
    bool createPerLevelResources(const VulkanContext& context);
    bool createImage(VkExtent2D storageExtent, VkImageView depthView);
    bool writePerLevelDescriptors(VkImageView depthView);
    void destroyImage();
    void recordSinglePass(VkCommandBuffer commandBuffer, glm::uvec2 baseExtent, uint32_t levels,
                          std::optional<DepthChangeCount> depthChanges);
    void recordPerLevel(VkCommandBuffer commandBuffer, glm::uvec2 baseExtent, uint32_t levels);
    void levelBarrier(VkCommandBuffer commandBuffer, uint32_t baseLevel, uint32_t levelCount) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    bool m_singlePass{false};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    // Per-level path: one descriptor set per level.
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_levelDescriptors;
    // Single-pass path: the workgroup counter that elects the last workgroup.
    // The shader returns it to zero at the end of every build.
    VkBuffer m_counter{VK_NULL_HANDLE};
    VmaAllocation m_counterAllocation{VK_NULL_HANDLE};
    bool m_counterInitialized{false};
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImage m_image{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkImageView m_view{VK_NULL_HANDLE};
    std::vector<VkImageView> m_levelViews;
    VkExtent2D m_storageExtent{};
    VkExtent2D m_renderExtent{};
    uint32_t m_builtLevels{0};
    bool m_layoutInitialized{false};
};
