#define VMA_IMPLEMENTATION
#include "VulkanContext.hpp"
#include "VulkanCheck.hpp"
#include <SDL3/SDL_vulkan.h>
#include <cstring>

namespace {
constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t invalidQueueFamily = UINT32_MAX;
constexpr uint32_t graphicsQueueIndex = 0;
constexpr float graphicsQueuePriority = 1.0f;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << data->pMessage << '\n';
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() {
    return {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = debugCallback};
}

std::vector<const char*> requestedValidationLayers() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, validationLayer) == 0) {
            std::cerr << "Vulkan validation enabled.\n";
            return {validationLayer};
        }
    }
    std::cerr << "Validation layer unavailable; continuing without validation.\n";
    return {};
}

bool requestedInstanceExtensions(std::vector<const char*>& extensions) {
    uint32_t count = 0;
    const char* const* windowExtensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!windowExtensions) {
        return false;
    }
    extensions.assign(windowExtensions, windowExtensions + count);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    return true;
}

bool supportsSwapchainExtension(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

bool supportsRenderingFeatures(VkPhysicalDevice device) {
    VkPhysicalDeviceVulkan13Features renderingFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &renderingFeatures};
    vkGetPhysicalDeviceFeatures2(device, &features);
    return renderingFeatures.dynamicRendering && renderingFeatures.synchronization2;
}

uint32_t findGraphicsQueueFamily(VkPhysicalDevice device, const Surface& surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t index = 0; index < count; ++index) {
        const bool supportsGraphics = (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (!supportsGraphics) {
            continue;
        }
        if (surface.supportsPresent(device, index)) {
            return index;
        }
    }
    return invalidQueueFamily;
}
} // namespace

bool VulkanContext::initInstance() {
    std::vector<const char*> extensions;
    if (!requestedInstanceExtensions(extensions)) {
        return false;
    }
    const std::vector<const char*> layers = requestedValidationLayers();
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerInfo();
    VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Zenith", .apiVersion = VulkanVersion};
    VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pNext = &debugInfo,
                                      .pApplicationInfo = &application,
                                      .enabledLayerCount = static_cast<uint32_t>(layers.size()),
                                      .ppEnabledLayerNames = layers.data(),
                                      .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
                                      .ppEnabledExtensionNames = extensions.data()};
    if (!checkVulkan(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "Create instance")) {
        return false;
    }
    volkLoadInstance(m_instance);
    return checkVulkan(vkCreateDebugUtilsMessengerEXT(m_instance, &debugInfo, nullptr, &m_debugMessenger),
                       "Create debug messenger");
}

bool VulkanContext::supportsDevice(VkPhysicalDevice device, const Surface& surface) const {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VulkanVersion) {
        return false;
    }
    if (!supportsRenderingFeatures(device)) {
        return false;
    }
    if (!supportsSwapchainExtension(device)) {
        return false;
    }
    if (findGraphicsQueueFamily(device, surface) == invalidQueueFamily) {
        return false;
    }
    if (surface.formats(device).empty()) {
        return false;
    }
    return !surface.presentModes(device).empty();
}

bool VulkanContext::initPhysicalDevice(const Surface& surface) {
    uint32_t count = 0;
    if (!checkVulkan(vkEnumeratePhysicalDevices(m_instance, &count, nullptr), "Count physical devices")) {
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!checkVulkan(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()), "Get physical devices")) {
        return false;
    }
    for (VkPhysicalDevice candidate : devices) {
        if (!supportsDevice(candidate, surface)) {
            continue;
        }
        m_pDevice = candidate;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            break;
        }
    }
    if (!m_pDevice) {
        std::cerr << "No suitable Vulkan 1.4 graphics/presentation device.\n";
        return false;
    }
    return true;
}

bool VulkanContext::initGfxQueue(const Surface& surface) {
    m_gfxQueueFamIdx = findGraphicsQueueFamily(m_pDevice, surface);
    return m_gfxQueueFamIdx != invalidQueueFamily;
}

bool VulkanContext::initDevice() {
    if (m_gfxQueueFamIdx == invalidQueueFamily) {
        return false;
    }
    VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = m_gfxQueueFamIdx,
                                  .queueCount = 1,
                                  .pQueuePriorities = &graphicsQueuePriority};
    VkPhysicalDeviceVulkan13Features features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                              .synchronization2 = VK_TRUE,
                                              .dynamicRendering = VK_TRUE};
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                  .pNext = &features,
                                  .queueCreateInfoCount = 1,
                                  .pQueueCreateInfos = &queue,
                                  .enabledExtensionCount = 1,
                                  .ppEnabledExtensionNames = &swapchainExtension};
    if (!checkVulkan(vkCreateDevice(m_pDevice, &deviceInfo, nullptr, &m_device), "Create device")) {
        return false;
    }
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_gfxQueueFamIdx, graphicsQueueIndex, &m_gfxQueue);
    return m_gfxQueue != VK_NULL_HANDLE;
}

bool VulkanContext::initVMA() {
    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo info{.physicalDevice = m_pDevice,
                                .device = m_device,
                                .pVulkanFunctions = &functions,
                                .instance = m_instance,
                                .vulkanApiVersion = VulkanVersion};
    if (!checkVulkan(vmaImportVulkanFunctionsFromVolk(&info, &functions), "Import allocator functions")) {
        return false;
    }
    return checkVulkan(vmaCreateAllocator(&info, &m_vmaAllocator), "Create allocator");
}

void VulkanContext::closeDevice() {
    if (m_vmaAllocator) {
        vmaDestroyAllocator(m_vmaAllocator);
    }
    m_vmaAllocator = VK_NULL_HANDLE;
    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_gfxQueue = VK_NULL_HANDLE;
}

void VulkanContext::closeInstance() {
    if (m_debugMessenger) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }
    m_debugMessenger = VK_NULL_HANDLE;
    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
    }
    m_instance = VK_NULL_HANDLE;
    volkFinalize();
}
