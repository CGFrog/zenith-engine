#pragma once

#include "../vulkan/VulkanContext.hpp"

// Copy of the opaque scene (with its background and screen-space reflections)
// that transmissive surfaces (KHR_materials_transmission) sample before they
// draw over it. The scene pass captures it between opaque and blended
// surfaces, only on frames that draw a transmissive surface.
//
// Contract for readers (mesh.frag):
// - One HDR image of storageExtent with a full mip chain, always in
//   readLayout once prepareFirstUse() ran. Level 0 holds the render-extent
//   region of the scene stretched over the whole image, so a fragment's uv is
//   gl_FragCoord.xy / render extent and every level is fully valid.
// - Higher levels are box-filtered for rough transmission (lod grows with
//   roughness). Sample with sampler(): trilinear, clamped.
// - Contents are stale on frames that captured nothing; only shaders of
//   transmissive surfaces drawn after capture() read them.
class TransmissionBackground {
  public:
    // Matches PostProcessPass::sceneFormat, the blit source.
    static constexpr VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageLayout readLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    bool init(const VulkanContext& context, VkExtent2D storageExtent);
    // Recreates storage; the device must be idle.
    bool resize(VkExtent2D storageExtent);
    // Moves a new image into readLayout so descriptors that statically
    // reference it are valid before the first capture.
    void prepareFirstUse(VkCommandBuffer commandBuffer);
    // Copies renderExtent of the scene color and rebuilds the mip chain.
    // Called outside rendering; the scene image must be in
    // COLOR_ATTACHMENT_OPTIMAL with its attachment writes pending, and is
    // returned to that layout with the copy ordered before later attachment
    // access. The copy is readable by fragment shaders afterwards.
    void capture(VkCommandBuffer commandBuffer, VkImage sceneImage, VkExtent2D renderExtent);
    void shutdown();

    VkImageView view() const {
        return m_view;
    }

    VkSampler sampler() const {
        return m_sampler;
    }

    uint32_t levelCount() const {
        return m_levels;
    }

  private:
    bool createImage(VkExtent2D storageExtent);
    void destroyImage();

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkImage m_image{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkImageView m_view{VK_NULL_HANDLE};
    VkExtent2D m_storageExtent{};
    uint32_t m_levels{0};
    bool m_layoutInitialized{false};
};
