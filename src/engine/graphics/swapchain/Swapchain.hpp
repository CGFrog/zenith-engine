#pragma once
#include <volk.h>
#include "../../window/Window.hpp"
#include "../surface/Surface.hpp"
#include "../vulkan/VulkanContext.hpp"
class Swapchain{
public:

constexpr static VkFormat swapchainFormat{ VK_FORMAT_B8G8R8A8_SRGB };
constexpr static VkFormat depthFormat{ VK_FORMAT_D32_SFLOAT };

bool initSwapchain(const Window& window, const Surface& surface, const VulkanContext& context);
void destroySwapchain(const VulkanContext& context);
const std::vector<VkImage>& swapchainImages() const;
const std::vector<VkImageView>& swapchainImageViews() const;
private:
VkSwapchainKHR m_swapchain {VK_NULL_HANDLE};
uint32_t m_scWidth{0};
uint32_t m_scHeight{0};

// Images are the actual stored memory and image views tell Vulkan how to understand the memory similar to sType.
std::vector<VkImage> m_swapchainImages{};
std::vector<VkImageView> m_swapchainImageViews{};

VkImage m_depthImage {VK_NULL_HANDLE};
VkImageView m_depthImageView {VK_NULL_HANDLE};
VmaAllocation m_depthImageAllocation {VK_NULL_HANDLE};
};