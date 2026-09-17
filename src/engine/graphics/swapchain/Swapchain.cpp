#include "Swapchain.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include <algorithm>
#include <limits>

namespace {
constexpr uint32_t extraSwapchainImages = 1;
constexpr uint64_t acquireTimeout = std::numeric_limits<uint64_t>::max();

VkCompositeAlphaFlagBitsKHR selectCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps) {
    constexpr VkCompositeAlphaFlagBitsKHR preferences[]{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (VkCompositeAlphaFlagBitsKHR candidate : preferences) {
        if (caps.supportedCompositeAlpha & candidate) {
            return candidate;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
} // namespace

bool Swapchain::initSwapchain(const Window& window, const Surface& surface, const VulkanContext& context) {
    const VkSurfaceCapabilitiesKHR caps = surface.capabilities(context.pDevice());
    if (!selectExtent(window, caps)) {
        return false;
    }
    VkSurfaceFormatKHR format{};
    if (!selectFormat(surface, context.pDevice(), format)) {
        return false;
    }
    selectPresentMode(surface, context.pDevice());
    if (!createHandle(context.device(), surface.handle(), caps, format)) {
        return false;
    }
    if (!createImageViews(context.device())) {
        return false;
    }
    if (!createPresentationSemaphores(context.device())) {
        return false;
    }
    return createDepthResources(context);
}

bool Swapchain::selectExtent(const Window& window, const VkSurfaceCapabilitiesKHR& caps) {
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window.handle(), &width, &height)) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    constexpr uint32_t variableExtent = std::numeric_limits<uint32_t>::max();
    if (caps.currentExtent.width != variableExtent) {
        m_extent = caps.currentExtent;
    } else {
        m_extent = {
            .width = std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width),
            .height =
                std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    if (m_extent.width == 0 || m_extent.height == 0) {
        return false;
    }
    return (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
}

bool Swapchain::selectFormat(const Surface& surface, VkPhysicalDevice device, VkSurfaceFormatKHR& format) {
    const std::vector<VkSurfaceFormatKHR> formats = surface.formats(device);
    if (formats.empty()) {
        return false;
    }
    format = formats.front();
    for (const VkSurfaceFormatKHR& candidate : formats) {
        const bool preferredFormat = candidate.format == swapchainFormat;
        const bool preferredColorSpace = candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        if (preferredFormat && preferredColorSpace) {
            format = candidate;
            break;
        }
    }
    if (format.format == VK_FORMAT_UNDEFINED) {
        format.format = swapchainFormat;
    }
    m_format = format.format;
    return true;
}

void Swapchain::selectPresentMode(const Surface& surface, VkPhysicalDevice device) {
    const std::vector<VkPresentModeKHR> modes = surface.presentModes(device);
    m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            m_presentMode = mode;
        }
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            m_presentMode = mode;
            break;
        }
    }
}

const char* Swapchain::presentModeName() const {
    switch (m_presentMode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "VSync off";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "Mailbox";
    default:
        return "VSync on";
    }
}

bool Swapchain::createHandle(VkDevice device, VkSurfaceKHR surface, const VkSurfaceCapabilitiesKHR& caps,
                             VkSurfaceFormatKHR format) {
    uint32_t imageCount = caps.minImageCount + extraSwapchainImages;
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                  .surface = surface,
                                  .minImageCount = imageCount,
                                  .imageFormat = format.format,
                                  .imageColorSpace = format.colorSpace,
                                  .imageExtent = m_extent,
                                  .imageArrayLayers = 1,
                                  .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  .preTransform = caps.currentTransform,
                                  .compositeAlpha = selectCompositeAlpha(caps),
                                  .presentMode = m_presentMode,
                                  .clipped = VK_TRUE};
    return checkVulkan(vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain), "Create swapchain");
}

bool Swapchain::createImageViews(VkDevice device) {
    uint32_t imageCount = 0;
    if (!checkVulkan(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, nullptr), "Count swapchain images")) {
        return false;
    }
    m_swapchainImages.resize(imageCount);
    if (!checkVulkan(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, m_swapchainImages.data()),
                     "Get swapchain images")) {
        return false;
    }
    m_swapchainImageViews.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t index = 0; index < imageCount; ++index) {
        VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = m_swapchainImages[index],
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = m_format,
                                   .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .baseMipLevel = 0,
                                                        .levelCount = 1,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1}};
        if (!checkVulkan(vkCreateImageView(device, &info, nullptr, &m_swapchainImageViews[index]),
                         "Create swapchain image view")) {
            return false;
        }
    }
    return true;
}

bool Swapchain::createPresentationSemaphores(VkDevice device) {
    m_renderCompleteSemaphores.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& semaphore : m_renderCompleteSemaphores) {
        if (!checkVulkan(vkCreateSemaphore(device, &info, nullptr, &semaphore), "Create present semaphore")) {
            return false;
        }
    }
    return true;
}

bool Swapchain::createDepthResources(const VulkanContext& context) {
    constexpr VkFormat candidates[]{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    bool found = false;
    for (VkFormat candidate : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.pDevice(), candidate, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depthFormat = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = m_depthFormat,
                            .extent = {.width = m_extent.width, .height = m_extent.height, .depth = 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO};
    if (!checkVulkan(vmaCreateImage(context.vmaAllocator(), &image, &allocation, &m_depthImage, &m_depthImageAllocation,
                                    nullptr),
                     "Create depth image")) {
        return false;
    }
    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = m_depthImage,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = m_depthFormat,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                    .baseMipLevel = 0,
                                                    .levelCount = 1,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1}};
    return checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &m_depthImageView),
                       "Create depth image view");
}

VkResult Swapchain::acquire(VkDevice device, VkSemaphore imageAcquired, uint32_t& imageIndex) const {
    return vkAcquireNextImageKHR(device, m_swapchain, acquireTimeout, imageAcquired, VK_NULL_HANDLE, &imageIndex);
}

VkResult Swapchain::present(VkQueue queue, uint32_t imageIndex) const {
    const VkSemaphore renderFinished = renderComplete(imageIndex);
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                          .waitSemaphoreCount = 1,
                          .pWaitSemaphores = &renderFinished,
                          .swapchainCount = 1,
                          .pSwapchains = &m_swapchain,
                          .pImageIndices = &imageIndex};
    return vkQueuePresentKHR(queue, &info);
}

void Swapchain::destroyDepthResources(const VulkanContext& context) {
    if (m_depthImageView) {
        vkDestroyImageView(context.device(), m_depthImageView, nullptr);
    }
    if (m_depthImage) {
        vmaDestroyImage(context.vmaAllocator(), m_depthImage, m_depthImageAllocation);
    }
    m_depthImageView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthImageAllocation = VK_NULL_HANDLE;
}

void Swapchain::destroySwapchain(const VulkanContext& context) {
    destroyDepthResources(context);
    for (VkSemaphore semaphore : m_renderCompleteSemaphores) {
        if (semaphore) {
            vkDestroySemaphore(context.device(), semaphore, nullptr);
        }
    }
    m_renderCompleteSemaphores.clear();
    for (VkImageView view : m_swapchainImageViews) {
        if (view) {
            vkDestroyImageView(context.device(), view, nullptr);
        }
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();
    if (m_swapchain) {
        vkDestroySwapchainKHR(context.device(), m_swapchain, nullptr);
    }
    m_swapchain = VK_NULL_HANDLE;
}
