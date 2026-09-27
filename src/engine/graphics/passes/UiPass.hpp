#pragma once

#include "../../window/Window.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <SDL3/SDL_events.h>
#include <cstdint>

class UiPass {
  public:
    bool init(const VulkanContext& context, const Window& window, VkFormat colorFormat, uint32_t imageCount);
    void processEvent(const SDL_Event& event);
    void beginFrame();
    void cancelFrame();
    bool record(VkCommandBuffer commandBuffer, VkImage image, VkImageView imageView, VkExtent2D extent,
                bool clearColor = false);
    uint64_t addTexture(VkSampler sampler, VkImageView view);
    void removeTexture(uint64_t texture);
    bool wantsKeyboardCapture() const;
    bool wantsMouseCapture() const;
    void shutdown();

  private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    bool m_initialized{false};
    bool m_frameActive{false};
};
