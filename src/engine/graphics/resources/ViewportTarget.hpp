#pragma once

// Offscreen color/depth target a render view draws into (editor viewports).

#include "../passes/UiPass.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <cstdint>
#include <vector>

class ViewportTarget {
  public:
    bool init(const VulkanContext& context, UiPass& uiPass, VkFormat colorFormat, VkFormat depthFormat,
              uint32_t frameCount, VkExtent2D extent);
    bool resize(const VulkanContext& context, UiPass& uiPass, VkExtent2D extent);
    void transitionForSampling(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    void markWritten(uint32_t frameIndex);
    void shutdown(const VulkanContext& context, UiPass& uiPass);

    VkImage image(uint32_t frameIndex) const;
    VkImageView view(uint32_t frameIndex) const;
    VkImageLayout oldLayout(uint32_t frameIndex) const;
    uint64_t texture(uint32_t frameIndex) const;
    VkImage depthImage() const;
    VkImageView depthView() const;
    VkExtent2D extent() const;

  private:
    struct ColorTarget {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        uint64_t texture{0};
        bool initialized{false};
    };

    bool createImages(const VulkanContext& context, UiPass& uiPass, VkExtent2D extent);
    void destroyImages(const VulkanContext& context, UiPass& uiPass);

    VkFormat m_colorFormat{VK_FORMAT_UNDEFINED};
    VkFormat m_depthFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D m_extent{};
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkImage m_depthImage{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};
    VmaAllocation m_depthAllocation{VK_NULL_HANDLE};
    std::vector<ColorTarget> m_colors;
};
