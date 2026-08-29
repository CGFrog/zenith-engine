#pragma once
#include <volk.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>

class Surface {
public:
    bool init(VkInstance instance, SDL_Window* window);
    void shutdown(VkInstance instance);
    
    VkSurfaceKHR handle() const;
    VkSurfaceCapabilitiesKHR capabilities(VkPhysicalDevice physicalDevice) const;
    std::vector<VkSurfaceFormatKHR> formats(VkPhysicalDevice physicalDevice) const;
    std::vector<VkPresentModeKHR> presentModes( VkPhysicalDevice physicalDevice) const;
    bool supportsPresent(VkPhysicalDevice physicalDevice, uint32_t queueFamily) const;
    bool supportsFormat(VkPhysicalDevice physicalDevice, VkFormat swapchainFormat);
private:
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
};