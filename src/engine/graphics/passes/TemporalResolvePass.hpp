#pragma once

#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>

#include <array>
#include <vector>

// Camera and history state the resolve needs for one frame.
struct TemporalResolveFrame {
    // Maps the current frame's unjittered clip space to the previous frame's
    // unjittered clip space.
    glm::mat4 currentToPreviousClip{1.0f};
    // Offset of this frame's samples from internal texel centers, in internal
    // pixels (TemporalFrameState::jitterPixels).
    glm::vec2 jitterPixels{0.0f};
    bool historyValid{false};
};

// This frame's inputs. Scene color and motion are output-sized images whose
// top-left renderExtent texels hold the frame, sampled in
// SHADER_READ_ONLY_OPTIMAL. previousMotion is the previous frame's motion
// target, filled within previousRenderExtent; when no distinct previous target
// exists, pass the current one and clear previousMotionValid.
struct TemporalResolveImages {
    VkImageView sceneColor{VK_NULL_HANDLE};
    VkImageView motion{VK_NULL_HANDLE};
    VkImageView previousMotion{VK_NULL_HANDLE};
    VkExtent2D renderExtent{};
    VkExtent2D previousRenderExtent{};
    bool previousMotionValid{false};
};

// Temporal anti-aliasing and upsampling (shaders/taa_resolve.comp). Owns the
// accumulated history for one view: two output-resolution images used
// ping-pong, so each frame reads the previous frame's result and writes the
// other image. History is not per frame in flight: consecutive frames are
// submitted in order on one queue, and the barriers recorded here order each
// frame's history accesses after the previous frame's. The images are
// allocated on the first resolve after init or resize, stay in
// VK_IMAGE_LAYOUT_GENERAL, and are released on resize or shutdown.
class TemporalResolvePass {
  public:
    static constexpr VkFormat historyFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageLayout resolvedLayout = VK_IMAGE_LAYOUT_GENERAL;

    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent);
    // Releases history; the next resolve allocates it at the new output extent.
    // The caller must ensure the GPU no longer uses it.
    void resize(VkExtent2D outputExtent);
    void shutdown();

    // Resolves into the next history image and makes it readable by later
    // fragment and compute shaders. The caller must have finished writing the
    // input images with color-attachment or transfer operations and moved them
    // to SHADER_READ_ONLY_OPTIMAL for fragment-shader reads. Fails only if
    // history cannot be allocated.
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const TemporalResolveImages& images,
                const TemporalResolveFrame& frame);

    // The most recently resolved image in resolvedLayout, or VK_NULL_HANDLE
    // when nothing was resolved since init or the last resize.
    VkImageView resolvedView() const {
        return m_historyWritten ? m_history[m_latest].view : VK_NULL_HANDLE;
    }

    uint64_t historyBytes() const;

  private:
    struct HistoryImage {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool createDescriptors(uint32_t frameCount);
    // Allocates history at m_extent and moves it to resolvedLayout.
    bool ensureHistory(VkCommandBuffer commandBuffer);
    void destroyHistory();
    void writeDescriptors(VkDescriptorSet descriptor, const TemporalResolveImages& images, uint32_t readIndex,
                          uint32_t writeIndex) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    // Rewritten each frame; a frame slot's set is unused once its fence signals.
    std::vector<VkDescriptorSet> m_descriptors;
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    std::array<HistoryImage, 2> m_history{};
    VkExtent2D m_extent{};
    uint32_t m_latest{0};
    // True once m_latest holds a resolved frame at the current extent.
    bool m_historyWritten{false};
};
