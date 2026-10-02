#pragma once
#include "../../window/Window.hpp"
#include "../surface/Surface.hpp"
#include "../vulkan/VulkanContext.hpp"
#include <volk.h>

struct SwapchainOptions {
    // Adds TRANSFER_SRC usage so presented images can be copied to host memory
    // (frame capture). Creation fails when the surface does not support it.
    bool transferSource{false};
};

class Swapchain {
  public:
    static constexpr VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
    static constexpr VkFormat preferredDepthFormat = VK_FORMAT_D32_SFLOAT;

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool initSwapchain(const Window& window, const Surface& surface, const VulkanContext& context,
                       const SwapchainOptions& options = {});
    void destroySwapchain(const VulkanContext& context);
    VkResult acquire(VkDevice device, VkSemaphore imageAcquired, uint32_t& imageIndex) const;
    VkResult present(VkQueue queue, uint32_t imageIndex) const;

    VkSwapchainKHR handle() const {
        return m_swapchain;
    }

    VkExtent2D extent() const {
        return m_extent;
    }

    VkFormat format() const {
        return m_format;
    }

    VkFormat depthFormat() const {
        return m_depthFormat;
    }

    const char* presentModeName() const;

    VkSemaphore renderComplete(uint32_t imageIndex) const {
        return m_renderCompleteSemaphores[imageIndex];
    }

    const std::vector<VkImage>& swapchainImages() const {
        return m_swapchainImages;
    }

    uint32_t imageCount() const {
        return static_cast<uint32_t>(m_swapchainImages.size());
    }

    const std::vector<VkImageView>& swapchainImageViews() const {
        return m_swapchainImageViews;
    }

    const VkImage& depthImage() const {
        return m_depthImage;
    }

    const VkImageView& depthImageView() const {
        return m_depthImageView;
    }

  private:
    bool selectExtent(const Window& window, const VkSurfaceCapabilitiesKHR& caps);
    bool selectFormat(const Surface& surface, VkPhysicalDevice device, VkSurfaceFormatKHR& format);
    void selectPresentMode(const Surface& surface, VkPhysicalDevice device);
    bool createHandle(VkDevice device, VkSurfaceKHR surface, const VkSurfaceCapabilitiesKHR& caps,
                      VkSurfaceFormatKHR format, VkImageUsageFlags usage);
    bool createImageViews(VkDevice device);
    bool createPresentationSemaphores(VkDevice device);
    bool createDepthResources(const VulkanContext& context);
    void destroyDepthResources(const VulkanContext& context);

    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
    VkFormat m_format{swapchainFormat};
    VkFormat m_depthFormat{preferredDepthFormat};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D m_extent{.width = 0, .height = 0};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkSemaphore> m_renderCompleteSemaphores;

    VkImage m_depthImage{VK_NULL_HANDLE};
    VkImageView m_depthImageView{VK_NULL_HANDLE};
    VmaAllocation m_depthImageAllocation{VK_NULL_HANDLE};
};
