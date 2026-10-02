#pragma once

#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

// AMD FSR 1 spatial upscaling (SpatialUpscale.hpp documents the math and color
// space). The tone-mapped frame is composited at render resolution into the
// top-left region of an output-sized input target, EASU (shaders/fsr_easu.comp)
// upsamples that region into an output-resolution image, and RCAS
// (shaders/fsr_rcas.frag) sharpens it in a fullscreen draw into the output.
//
// Both targets are allocated on the first prepareInput() after init or resize
// and are shared by every frame in flight: frames are submitted in order on
// one queue and the barriers recorded here order each frame's accesses after
// the previous frame's. Their descriptors are written once per allocation.
class SpatialUpscalePass {
  public:
    // EASU's gamma-2 encoded output; RGBA8 storage is required by Vulkan.
    static constexpr VkFormat upscaledFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr uint32_t pipelineCount = 3;

    // outputFormat is the format of the post-process output and of the input
    // target, so the post-process pipeline can composite into either.
    // RCAS can also sharpen into an intermediate of gradeFormat (the colour
    // grade's display target). Succeeds without pipelines when the formats
    // lack the required features; see supported().
    bool init(const VulkanContext& context, VkFormat outputFormat, VkFormat gradeFormat, VkExtent2D outputExtent);
    // Releases the targets; the next prepareInput() allocates them at the new
    // extent. The caller must ensure the GPU no longer uses them.
    void resize(VkExtent2D outputExtent);
    void shutdown();

    bool supported() const {
        return m_supported;
    }

    // Moves the input target to COLOR_ATTACHMENT_OPTIMAL, discarding its
    // contents, and returns its view; VK_NULL_HANDLE if allocation failed.
    VkImageView prepareInput(VkCommandBuffer commandBuffer);
    // Upsamples the input target's top-left inputExtent into the upscaled
    // image. The input must have been rendered as a color attachment since
    // prepareInput(). Records timestamps 47 and 48.
    void recordEasu(VkCommandBuffer commandBuffer, VkExtent2D inputExtent, VkQueryPool timestampQueries);
    // Sharpens the upscaled image into targetView, which must be in
    // COLOR_ATTACHMENT_OPTIMAL, cover outputExtent, and have the output or
    // grade format given to init (targetFormat). Records timestamps 49 and 50.
    void recordRcas(VkCommandBuffer commandBuffer, VkImageView targetView, VkFormat targetFormat, float sharpness,
                    VkQueryPool timestampQueries);
    // Writes timestamps 47 to 50 back to back for frames that do not upscale.
    static void writeSkippedTimestamps(VkCommandBuffer commandBuffer, VkQueryPool timestampQueries);

    uint64_t targetBytes() const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool createDescriptors();
    bool createRcasPipelineLayout();
    bool createRcasPipeline(VkFormat targetFormat, VkPipelineCache pipelineCache, VkPipeline& pipeline);
    bool createTarget(Target& target, VkFormat format, VkImageUsageFlags usage);
    bool ensureTargets();
    void writeDescriptors();
    void destroyTargets();

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkFormat m_outputFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D m_extent{};
    bool m_supported{false};
    VkDescriptorSetLayout m_easuLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_rcasLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkDescriptorSet m_easuDescriptor{VK_NULL_HANDLE};
    VkDescriptorSet m_rcasDescriptor{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_easuPipeline;
    VkPipelineLayout m_rcasPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_rcasPipeline{VK_NULL_HANDLE};
    // RCAS into the colour grade's display target.
    VkPipeline m_rcasGradePipeline{VK_NULL_HANDLE};
    Target m_input;
    Target m_upscaled;
    // True once a frame has used the targets since their allocation, so
    // barriers must wait for that frame's reads.
    bool m_targetsUsed{false};
};
