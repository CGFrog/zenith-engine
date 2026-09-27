#pragma once

#include "../Rendering.hpp"
#include "../vulkan/VulkanContext.hpp"
#include <vector>

class PostProcessPass {
  public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    bool init(const VulkanContext& context, VkFormat outputFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage, VkImageView outputView,
                VkImage depthImage, VkExtent2D extent, const RenderSettings& settings,
                const glm::mat4& inverseViewProjection, const glm::vec3& cameraPosition,
                const std::optional<GridOverlay>& gridOverlay,
                VkImageLayout outputOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void shutdown(const VulkanContext& context);

    VkImage sceneImage(uint32_t frameIndex) const;
    VkImageView sceneView(uint32_t frameIndex) const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        uint32_t mipLevels{1};
        bool initialized{false};
        bool mipsInitialized{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createPipeline(VkFormat outputFormat);
    bool createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void destroyTargets(const VulkanContext& context);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkSampler m_colorSampler{VK_NULL_HANDLE};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    std::vector<Target> m_targets;
};
