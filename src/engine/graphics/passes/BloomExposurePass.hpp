#pragma once

// Luminance histogram, exposure adaptation, and the bloom down/upsample chain.

#include "../Rendering.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <array>
#include <glm/glm.hpp>

// HDR input shared by the exposure and bloom computations. The image must be in
// `layout` and visible to compute shaders. Only the top-left validExtent of the
// imageExtent-sized image holds this frame's pixels (dynamic resolution renders
// into a sub-rectangle).
struct BloomExposureInput {
    VkImageView view{VK_NULL_HANDLE};
    VkExtent2D imageExtent{};
    VkExtent2D validExtent{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
};

// Maps a [0, 1] coordinate across the input's valid region to the bloom result:
// uv = min(coordinate * uvScale, uvLimit).
struct BloomSampling {
    glm::vec2 uvScale{1.0f};
    glm::vec2 uvLimit{1.0f};
};

// Computes auto exposure (log-luminance histogram plus temporal adaptation) and
// a mip-chain bloom from an HDR image.
//
// Exposure lives in one persistent storage buffer per pass instance (one per
// view), not per frame slot: each frame adapts from the value the previous frame
// wrote. That is safe with frames in flight because all frames execute in
// submission order on the graphics queue and every record call begins with a
// barrier against earlier readers. The bloom chain is shared across frame slots
// for the same reason. Consumers read exposureBuffer() as a std430 block of four
// floats (exposure, average log2 luminance, target exposure, padding) and
// bloomView() (mip 0, VK_IMAGE_LAYOUT_GENERAL) after the matching record call.
class BloomExposurePass {
  public:
    static constexpr VkFormat bloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t maximumBloomMips = 6;
    static constexpr VkDeviceSize exposureBufferSize = sizeof(float) * 4;

    bool init(const VulkanContext& context);
    // Allocates the bloom chain for inputs whose image extent is at most maximumExtent.
    bool createChain(const VulkanContext& context, VkExtent2D maximumExtent);
    void destroyChain(const VulkanContext& context);
    void shutdown(const VulkanContext& context);

    void recordExposure(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                        const ExposureSettings& settings, float deltaSeconds);
    void recordBloom(VkCommandBuffer commandBuffer, const BloomExposureInput& input, float filterRadius);
    // Moves a freshly created chain to GENERAL so bloomView() can stay bound while
    // bloom is disabled; its contents are then undefined and must not be read.
    void initializeBloomLayout(VkCommandBuffer commandBuffer);

    BloomSampling bloomSampling(VkExtent2D validExtent) const;

    VkImageView bloomView() const {
        return m_mipViews[0];
    }

    VkBuffer exposureBuffer() const {
        return m_exposure.buffer;
    }

  private:
    struct BufferAllocation {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool createLayouts();
    bool createPipelines(VkPipelineCache pipelineCache);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferAllocation& output);
    void pushSampledAndStorage(VkCommandBuffer commandBuffer, const ComputePipeline& pipeline, VkImageView sampled,
                               VkImageLayout sampledLayout, VkImageView storage);
    VkExtent2D allocatedMipExtent(uint32_t mip) const;
    VkExtent2D validMipExtent(VkExtent2D validExtent, uint32_t mip) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_histogramLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_adaptLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_bloomLayout{VK_NULL_HANDLE};
    ComputePipeline m_histogramPipeline;
    ComputePipeline m_adaptPipeline;
    ComputePipeline m_downsamplePipeline;
    ComputePipeline m_upsamplePipeline;
    BufferAllocation m_histogram;
    BufferAllocation m_exposure;
    bool m_exposureValid{false};

    VkImage m_bloomImage{VK_NULL_HANDLE};
    VmaAllocation m_bloomAllocation{VK_NULL_HANDLE};
    std::array<VkImageView, maximumBloomMips> m_mipViews{};
    VkExtent2D m_chainExtent{};
    uint32_t m_mipCount{0};
    bool m_chainInitialized{false};
};
