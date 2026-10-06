#pragma once

// Motion blur: tile max, neighbour max, reconstruction gather.

#include "engine/graphics/MotionBlur.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>

#include <vector>

// This frame's motion blur inputs. sceneColor is an HDR image whose top-left
// colorRegion texels hold the frame, sampled in sceneColorLayout; motion is
// the frame's motion-vector target (PostProcessPass::motionFormat), filled
// within motionRegion and sampled in SHADER_READ_ONLY_OPTIMAL. Both must have
// been made visible to compute shaders.
struct MotionBlurInput {
    VkImageView sceneColor{VK_NULL_HANDLE};
    VkImageLayout sceneColorLayout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkExtent2D colorRegion{};
    VkImageView motion{VK_NULL_HANDLE};
    VkExtent2D motionRegion{};
    // Maps the current frame's unjittered clip space to the previous frame's
    // (TemporalResolveFrame::currentToPreviousClip); reprojects the far plane
    // where no surface wrote motion.
    glm::mat4 currentToPreviousClip{1.0f};
};

// Motion blur reconstructed from the motion vectors (shaders/motion_blur_*.comp,
// math in MotionBlur.hpp). Owns an output-sized blurred colour image and two
// tile-velocity images for one view. Like the temporal history they are not
// per frame in flight: frames are submitted in order on one queue and the
// barriers recorded here order each frame's accesses after the previous
// frame's. The images are allocated on the first blur after init or resize,
// stay in VK_IMAGE_LAYOUT_GENERAL, and are released on resize or shutdown.
class MotionBlurPass {
  public:
    static constexpr VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat tileFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageLayout blurredLayout = VK_IMAGE_LAYOUT_GENERAL;

    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent);
    // Releases the images; the next blur allocates them at the new output
    // extent. The caller must ensure the GPU no longer uses them.
    void resize(VkExtent2D outputExtent);
    void shutdown();

    // Blurs input.colorRegion of the scene into the top-left of blurredView()
    // and makes it readable by later fragment and compute shaders. The plan
    // must have samples > 0. Fails only if the images cannot be allocated.
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const MotionBlurInput& input,
                const motion_blur::FramePlan& plan);

    // The blurred scene in blurredLayout, or VK_NULL_HANDLE before the first
    // blur since init or the last resize.
    VkImageView blurredView() const {
        return m_blurred.view;
    }

    uint64_t targetBytes() const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createTarget(Target& target, VkFormat format, VkExtent2D extent, const char* name);
    bool ensureTargets();
    void destroyTargets();
    void writeDescriptors(VkDescriptorSet descriptor, const MotionBlurInput& input) const;
    glm::uvec2 tileImageExtent() const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    // Rewritten each frame; a frame slot's set is unused once its fence signals.
    std::vector<VkDescriptorSet> m_descriptors;
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_tileMax;
    ComputePipeline m_neighborMax;
    ComputePipeline m_reconstruct;
    Target m_blurred;
    Target m_tileVelocity;
    Target m_neighborVelocity;
    VkExtent2D m_extent{};
};
