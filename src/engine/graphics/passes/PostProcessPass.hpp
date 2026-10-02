#pragma once

#include "../Rendering.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "BloomExposurePass.hpp"
#include "TemporalResolvePass.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

class PostProcessPass {
  public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat motionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Depth prepass output: octahedral view-space normal in rg, roughness in
    // b, coverage in a. SceneNormalEncoding.hpp documents the encoding.
    static constexpr VkFormat normalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

    bool init(const VulkanContext& context, VkFormat outputFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage, VkImageView outputView,
                VkImage depthImage, VkExtent2D extent, VkExtent2D internalExtent, const RenderSettings& settings,
                const glm::mat4& inverseViewProjection, const glm::mat4& previousViewProjection,
                const TemporalResolveFrame& temporal, const glm::vec3& cameraPosition,
                const std::optional<GridOverlay>& gridOverlay, VkQueryPool timestampQueries,
                double& bloomCpuMilliseconds, VkImageLayout outputOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void shutdown(const VulkanContext& context);

    VkImage sceneImage(uint32_t frameIndex) const;
    VkImageView sceneView(uint32_t frameIndex) const;
    VkImage motionImage(uint32_t frameIndex) const;
    VkImageView motionView(uint32_t frameIndex) const;
    // Moves the frame's normal target to COLOR_ATTACHMENT_OPTIMAL for the depth
    // prepass, discarding its previous contents.
    void prepareNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Makes the prepass normals readable (SHADER_READ_ONLY_OPTIMAL) by compute
    // and fragment shaders.
    void finishNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    VkImageView normalView(uint32_t frameIndex) const;

    uint64_t temporalHistoryBytes() const {
        return m_temporalResolve.historyBytes();
    }

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        bool initialized{false};
        VkImage motionImage{VK_NULL_HANDLE};
        VkImageView motionView{VK_NULL_HANDLE};
        VmaAllocation motionAllocation{VK_NULL_HANDLE};
        bool motionInitialized{false};
        VkImage reflectionImage{VK_NULL_HANDLE};
        VkImageView reflectionView{VK_NULL_HANDLE};
        VmaAllocation reflectionAllocation{VK_NULL_HANDLE};
        bool effectsInitialized{false};
        VkImage normalImage{VK_NULL_HANDLE};
        VkImageView normalView{VK_NULL_HANDLE};
        VmaAllocation normalAllocation{VK_NULL_HANDLE};
        bool normalInitialized{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createPipeline(VkFormat outputFormat);
    bool createEffectPipeline(const std::filesystem::path& fragmentPath, VkFormat outputFormat, VkPipeline& pipeline);
    bool createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void destroyTargets(const VulkanContext& context);
    // Runs the temporal resolve when enabled and binds the latest resolved image
    // as the post pass's binding 2. Records timestamps 19 and 20 either way.
    bool resolveTemporal(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkExtent2D internalExtent, bool enabled,
                         const TemporalResolveFrame& temporal, VkQueryPool timestampQueries);
    void recordBloomAndExposure(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                                const RenderSettings& settings, VkQueryPool timestampQueries,
                                double& bloomCpuMilliseconds);
    float exposureDeltaSeconds();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipeline m_reflectionPipeline{VK_NULL_HANDLE};
    VkSampler m_colorSampler{VK_NULL_HANDLE};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    std::vector<Target> m_targets;
    TemporalResolvePass m_temporalResolve;
    // The most recently recorded frame slot and its render extent; that slot's
    // motion target supplies the previous frame's motion and depth.
    std::optional<uint32_t> m_previousFrameIndex;
    VkExtent2D m_previousRenderExtent{};
    BloomExposurePass m_bloomExposure;
    std::optional<std::chrono::steady_clock::time_point> m_previousExposureTime;
};
