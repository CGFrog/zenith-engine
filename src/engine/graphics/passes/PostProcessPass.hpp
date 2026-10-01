#pragma once

#include "../Rendering.hpp"
#include "../vulkan/VulkanContext.hpp"
#include <filesystem>
#include <vector>

class PostProcessPass {
  public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat motionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat ambientOcclusionFormat = VK_FORMAT_R8_UNORM;

    bool init(const VulkanContext& context, VkFormat outputFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage, VkImageView outputView,
                VkImage depthImage, VkExtent2D extent, VkExtent2D internalExtent, const RenderSettings& settings,
                const glm::mat4& inverseViewProjection, const glm::mat4& previousViewProjection,
                bool temporalHistoryValid, const glm::vec3& cameraPosition,
                const std::optional<GridOverlay>& gridOverlay, VkQueryPool timestampQueries,
                double& bloomCpuMilliseconds, VkImageLayout outputOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void shutdown(const VulkanContext& context);

    VkImage sceneImage(uint32_t frameIndex) const;
    VkImageView sceneView(uint32_t frameIndex) const;
    VkImage motionImage(uint32_t frameIndex) const;
    VkImageView motionView(uint32_t frameIndex) const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        uint32_t mipLevels{1};
        bool initialized{false};
        bool mipsInitialized{false};
        VkImage motionImage{VK_NULL_HANDLE};
        VkImageView motionView{VK_NULL_HANDLE};
        VmaAllocation motionAllocation{VK_NULL_HANDLE};
        bool motionInitialized{false};
        VkImage ambientOcclusionImage{VK_NULL_HANDLE};
        VkImageView ambientOcclusionView{VK_NULL_HANDLE};
        VmaAllocation ambientOcclusionAllocation{VK_NULL_HANDLE};
        VkImage reflectionImage{VK_NULL_HANDLE};
        VkImageView reflectionView{VK_NULL_HANDLE};
        VmaAllocation reflectionAllocation{VK_NULL_HANDLE};
        bool effectsInitialized{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createPipeline(VkFormat outputFormat);
    bool createEffectPipeline(const std::filesystem::path& fragmentPath, VkFormat outputFormat, VkPipeline& pipeline);
    bool createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void destroyTargets(const VulkanContext& context);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipeline m_ambientOcclusionPipeline{VK_NULL_HANDLE};
    VkPipeline m_reflectionPipeline{VK_NULL_HANDLE};
    VkSampler m_colorSampler{VK_NULL_HANDLE};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    std::vector<Target> m_targets;
};
