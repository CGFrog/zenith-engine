#pragma once

#include "../../assets/Assets.hpp"
#include "../../ui/GameUi.hpp"
#include "../../window/Window.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <SDL3/SDL_events.h>
#include <cstdint>
#include <functional>
#include <unordered_map>

class UiPass {
  public:
    bool init(const VulkanContext& context, const Window& window, VkFormat colorFormat, uint32_t imageCount);
    void processEvent(const SDL_Event& event);
    void beginFrame();
    void syncFonts(const AssetManager& assets);
    void drawGameUi(const GameUiContext& gameUi, const std::function<uint64_t(TextureHandle)>& textureResolver);
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
    // ImGui colors are sRGB-encoded; an sRGB target would encode them again.
    bool m_linearizeVertexColors{false};
    std::unordered_map<uint64_t, void*> m_fonts;
};
