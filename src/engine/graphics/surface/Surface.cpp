#include "Surface.hpp"

VkSurfaceKHR Surface::handle() const {
    return m_surface;
}

bool Surface::init(VkInstance instance, SDL_Window* window) {
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, &m_surface);
}

void Surface::shutdown(VkInstance instance) {
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance,m_surface,nullptr);
        m_surface = VK_NULL_HANDLE;
    }
}
VkSurfaceCapabilitiesKHR Surface::capabilities(VkPhysicalDevice physicalDevice) const {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_surface, &caps);
    return caps;
}

std::vector<VkSurfaceFormatKHR> Surface::formats(VkPhysicalDevice physicalDevice) const {
    uint32_t count{0};
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> result(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &count, result.data());
    return result;
}

std::vector<VkPresentModeKHR>
Surface::presentModes(VkPhysicalDevice physicalDevice) const {
    uint32_t count{0};
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,m_surface,&count,nullptr);
    std::vector<VkPresentModeKHR> result(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &count, result.data());
    return result;
}

bool Surface::supportsPresent(VkPhysicalDevice physicalDevice,uint32_t queueFamily) const {
    VkBool32 supported{VK_FALSE};
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamily, m_surface, &supported);
    return supported == VK_TRUE;
}

bool Surface::supportsFormat(VkPhysicalDevice physicalDevice, VkFormat swapchainFormat){
    for (const VkSurfaceFormatKHR &surfFormat : this->formats(physicalDevice)) {
        if (surfFormat.format == swapchainFormat) {
            return true;
        }
    }
    return false;
}
