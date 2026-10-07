#include "support/VulkanProbe.hpp"

#include <volk.h>

#include <vector>

namespace testsupport {

bool vulkanDeviceAvailable(VulkanDeviceKind kind, std::string& reason) {
    if (volkInitialize() != VK_SUCCESS) {
        reason = "no Vulkan loader";
        return false;
    }
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pApplicationName = "zenith_test_vulkan_probe",
                                        .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        reason = "cannot create a Vulkan instance";
        return false;
    }
    volkLoadInstanceOnly(instance);
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    bool found = false;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        found = found || kind == VulkanDeviceKind::Any || properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
    }
    vkDestroyInstance(instance, nullptr);
    reason = kind == VulkanDeviceKind::Any ? "no Vulkan device" : "no hardware Vulkan device";
    return found;
}

} // namespace testsupport
