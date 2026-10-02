#pragma once

#include "../AmbientOcclusion.hpp"
#include "../Rendering.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>

#include <vector>

// Depth-prepass outputs and camera terms GTAO reads. The normals must be in
// SHADER_READ_ONLY_OPTIMAL and the pyramid in GENERAL, both with their writes
// visible to compute (MeshPass::sceneGeometry documents the encodings).
struct AmbientOcclusionInput {
    VkImageView normals{VK_NULL_HANDLE};
    VkImageView depthPyramid{VK_NULL_HANDLE};
    VkSampler depthPyramidSampler{VK_NULL_HANDLE};
    VkExtent2D renderExtent{};
    // Inverse of the (jittered) projection that produced the depth.
    glm::mat4 inverseProjection{1.0f};
    // Render pixels per view unit at view distance 1 (0.5 * height * |P[1][1]|).
    float projectionScale{1.0f};
    // Varies the sampling noise per frame; only useful when a temporal resolve
    // accumulates the result, otherwise the noise would crawl.
    bool temporalNoise{false};
};

// Ground-truth ambient occlusion (shaders/gtao.comp), run after the depth
// prepass so the shading pass can apply it to indirect lighting only. Full
// resolution follows it with a depth-aware spatial denoise
// (shaders/gtao_denoise.comp); half resolution traces a quarter of the texels
// with a cheaper sampling pattern (ambient_occlusion::qualityProfile) and then
// denoises and upsamples to the render resolution in one depth-aware pass
// (shaders/gtao_upsample.comp).
//
// Contract for readers:
// - occlusionView(frame) is an R8 visibility image (1 = unoccluded) that stays
//   in occlusionLayout. It is written per frame slot by record() or
//   recordUnoccluded() and readable by fragment shaders afterwards.
// - Contents cover resolutionScale(quality) texels per render pixel in the
//   top-left of a storage-sized image, plus one guard texel; sample with
//   sampler() at ambient_occlusion::occlusionUv. When nothing is occluded the
//   image is uniformly 1, so any scale reads correctly.
// - Temporal stability comes from the temporal resolve, not from this pass.
class AmbientOcclusionPass {
  public:
    static constexpr VkFormat occlusionFormat = VK_FORMAT_R8_UNORM;
    // Raw visibility and positive view depth between the two dispatches.
    static constexpr VkFormat rawFormat = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkImageLayout occlusionLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Texels per render pixel in occlusionView(). Every quality upsamples to
    // the render resolution, so this is 1; GTAO's own resolution is
    // ambient_occlusion::qualityProfile(quality).resolutionScale.
    static float resolutionScale(AmbientOcclusionQuality quality);

    bool init(const VulkanContext& context, uint32_t frameCount, VkExtent2D storageExtent);
    // Recreates the images for a new output size; the device must be idle and
    // descriptors referencing occlusionView() must be rewritten.
    bool resize(VkExtent2D storageExtent);
    // Computes the frame slot's occlusion at `quality` (not Off).
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, AmbientOcclusionQuality quality,
                const AmbientOcclusionInput& input);
    // Fills the frame slot's occlusion with 1, used when occlusion is disabled
    // or the depth prepass (and so its normals) did not run.
    void recordUnoccluded(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    void shutdown();

    VkImageView occlusionView(uint32_t frameIndex) const {
        return m_frames[frameIndex].view;
    }

    // Bilinear, clamp-to-edge.
    VkSampler sampler() const {
        return m_linearSampler;
    }

    uint64_t allocatedBytes() const;

    // Zero when the device cannot write the storage formats; record() then
    // behaves like recordUnoccluded().
    uint32_t pipelineCount() const {
        return m_computeSupported ? 3u : 0u;
    }

  private:
    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    enum class Contents {
        Undefined,
        Unoccluded,
        Occlusion,
    };

    struct FrameOcclusion : Image {
        Contents contents{Contents::Undefined};
    };

    bool createLayouts();
    bool createPipelines(VkPipelineCache pipelineCache);
    bool createImages(VkExtent2D storageExtent);
    bool createImage(VkFormat format, VkImageUsageFlags usage, const char* name, Image& output);
    void destroyImage(Image& image);
    void destroyImages();
    void recordGtao(VkCommandBuffer commandBuffer, const ambient_occlusion::GtaoQualityProfile& profile,
                    glm::uvec2 dispatchExtent, const AmbientOcclusionInput& input);
    void recordDenoise(VkCommandBuffer commandBuffer, uint32_t frameIndex, glm::uvec2 dispatchExtent,
                       float occlusionPower);
    void recordUpsample(VkCommandBuffer commandBuffer, uint32_t frameIndex, glm::uvec2 rawExtent, float occlusionPower,
                        const AmbientOcclusionInput& input);
    // Moves the frame slot's occlusion image to a compute write after its
    // previous reader or clear.
    void prepareOcclusionWrite(VkCommandBuffer commandBuffer, FrameOcclusion& frame);
    // Makes the compute write visible to the shading pass.
    void finishOcclusionWrite(VkCommandBuffer commandBuffer, FrameOcclusion& frame);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_gtaoLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_denoiseLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_upsampleLayout{VK_NULL_HANDLE};
    VkSampler m_pointSampler{VK_NULL_HANDLE};
    VkSampler m_linearSampler{VK_NULL_HANDLE};
    ComputePipeline m_gtaoPipeline;
    ComputePipeline m_denoisePipeline;
    ComputePipeline m_upsamplePipeline;
    bool m_computeSupported{false};
    VkExtent2D m_storageExtent{};
    // Shared by every frame slot: it lives only between GTAO and the denoise
    // or upsample dispatch, and queue-ordered barriers keep consecutive frames
    // from overlapping.
    Image m_raw;
    bool m_rawInitialized{false};
    std::vector<FrameOcclusion> m_frames;
    uint32_t m_noiseFrame{0};
};
