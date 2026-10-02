#pragma once

#include "engine/graphics/DepthOfField.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <array>
#include <vector>

// This frame's inputs. Both images are output-sized; the top-left
// colorExtent texels of color and depthExtent texels of motionDepth hold the
// frame (they differ when TAA upsamples the color to output resolution).
// color is in colorLayout and motionDepth in SHADER_READ_ONLY_OPTIMAL, both
// already visible to compute shaders. motionDepth is the motion target, whose
// alpha holds reversed-Z depth.
struct DepthOfFieldImages {
    VkImageView color{VK_NULL_HANDLE};
    VkImageLayout colorLayout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkExtent2D colorExtent{};
    VkImageView motionDepth{VK_NULL_HANDLE};
    VkExtent2D depthExtent{};
};

// Thin-lens depth of field on the HDR scene (shaders/dof_prepare.comp,
// dof_gather.comp, dof_composite.comp): computes each pixel's circle of
// confusion from depth, gathers a bokeh blur at half resolution, and blends it
// over the sharp scene into an output-resolution image. DepthOfField.hpp
// documents the math.
//
// The gather targets and the output are not per frame in flight: consecutive
// frames are submitted in order on one queue and the barriers recorded here
// order each frame's accesses after the previous frame's readers. They are
// allocated on the first record after init or resize, stay in
// VK_IMAGE_LAYOUT_GENERAL, and are released on resize or shutdown, so a view
// that never enables depth of field costs no memory (turning it Off keeps the
// targets until the next resize).
class DepthOfFieldPass {
  public:
    static constexpr VkFormat imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    static constexpr uint32_t pipelineCount() {
        return 3;
    }

    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent);
    // Releases the targets; the next record allocates them at the new extent.
    // The caller must ensure the GPU no longer uses them.
    void resize(VkExtent2D outputExtent);
    void shutdown();

    // Writes the focused scene into outputView() within images.colorExtent and
    // makes it readable by later fragment and compute shaders. The quality must
    // not be Off. Fails only if the targets cannot be allocated.
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const DepthOfFieldImages& images,
                const DepthOfFieldSettings& settings, const depth_of_field::CameraTerms& camera);

    // The output-sized focused scene in outputLayout, or VK_NULL_HANDLE before
    // the first record since init or the last resize.
    VkImageView outputView() const {
        return m_output.view;
    }

    uint64_t allocatedBytes() const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createTarget(Target& target, VkExtent2D extent);
    bool ensureTargets();
    void destroyTargets();
    void writeDescriptors(VkDescriptorSet descriptor, const DepthOfFieldImages& images) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    // Rewritten each frame; a frame slot's set is unused once its fence signals.
    std::vector<VkDescriptorSet> m_descriptors;
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_prepare;
    ComputePipeline m_gather;
    ComputePipeline m_composite;
    VkExtent2D m_extent{};
    // Half resolution: the downsampled scene with its CoC, and the gathered blur.
    Target m_prepared;
    Target m_gathered;
    Target m_output;
    // Whether the targets hold an earlier frame's accesses that the next
    // record must wait for.
    bool m_targetsUsed{false};
};
