#define VMA_IMPLEMENTATION
#include "VulkanContext.hpp"
#include "VulkanCheck.hpp"
#include "engine/platform/PlatformServices.hpp"
#include <SDL3/SDL_vulkan.h>
#include <cstring>
#include <fstream>

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
    VkPhysicalDeviceVulkan12Features indexingFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features renderingFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                       .pNext = &indexingFeatures};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &renderingFeatures};
    vkGetPhysicalDeviceFeatures2(device, &features);
    // Materials are bound through one runtime-sized, non-uniformly indexed
    // texture array, so these descriptor-indexing features are mandatory.
    const bool bindlessTextures = indexingFeatures.runtimeDescriptorArray &&
                                  indexingFeatures.descriptorBindingPartiallyBound &&
                                  indexingFeatures.descriptorBindingVariableDescriptorCount &&
                                  indexingFeatures.shaderSampledImageArrayNonUniformIndexing;
    return renderingFeatures.dynamicRendering && renderingFeatures.synchronization2 &&
           features.features.samplerAnisotropy && bindlessTextures;
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

uint32_t findTransferQueueFamily(VkPhysicalDevice device, uint32_t graphicsFamily) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t index = 0; index < count; ++index) {
        const bool transfer = (families[index].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        const bool graphics = (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (transfer && !graphics) {
            return index;
        }
    }
    return graphicsFamily;
}

std::vector<uint8_t> readPipelineCache(const std::filesystem::path& path) {
    constexpr std::streamsize maximumPipelineCacheBytes = 64 * 1024 * 1024;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamsize size = input.tellg();
    if (size <= 0 || size > maximumPipelineCacheBytes) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
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
        std::cerr << "No suitable Vulkan 1.4 graphics/presentation device with dynamic rendering, "
                     "synchronization2, and bindless texture support.\n";
        return false;
    }
    return true;
}

bool VulkanContext::initGfxQueue(const Surface& surface) {
    m_gfxQueueFamIdx = findGraphicsQueueFamily(m_pDevice, surface);
    m_transferQueueFamIdx = findTransferQueueFamily(m_pDevice, m_gfxQueueFamIdx);
    return m_gfxQueueFamIdx != invalidQueueFamily;
}

bool VulkanContext::initDevice() {
    if (m_gfxQueueFamIdx == invalidQueueFamily) {
        return false;
    }
    std::vector<VkDeviceQueueCreateInfo> queues{{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                 .queueFamilyIndex = m_gfxQueueFamIdx,
                                                 .queueCount = 1,
                                                 .pQueuePriorities = &graphicsQueuePriority}};
    if (m_transferQueueFamIdx != m_gfxQueueFamIdx) {
        queues.push_back({.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                          .queueFamilyIndex = m_transferQueueFamIdx,
                          .queueCount = 1,
                          .pQueuePriorities = &graphicsQueuePriority});
    }
    VkPhysicalDeviceVulkan12Features supported12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supported{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported12};
    vkGetPhysicalDeviceFeatures2(m_pDevice, &supported);
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .drawIndirectCount = supported12.drawIndirectCount,
        .descriptorIndexing = supported12.descriptorIndexing,
        .shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing,
        .descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind,
        .descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound,
        .descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount,
        .runtimeDescriptorArray = supported12.runtimeDescriptorArray,
        .timelineSemaphore = supported12.timelineSemaphore,
    };
    m_timelineSemaphoreSupported = supported12.timelineSemaphore == VK_TRUE;
    VkPhysicalDeviceVulkan13Features vulkan13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                              .pNext = &vulkan12,
                                              .synchronization2 = VK_TRUE,
                                              .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures enabledFeatures{.multiDrawIndirect = supported.features.multiDrawIndirect,
                                             .samplerAnisotropy = VK_TRUE};
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                  .pNext = &vulkan13,
                                  .queueCreateInfoCount = static_cast<uint32_t>(queues.size()),
                                  .pQueueCreateInfos = queues.data(),
                                  .enabledExtensionCount = 1,
                                  .ppEnabledExtensionNames = &swapchainExtension,
                                  .pEnabledFeatures = &enabledFeatures};
    if (!checkVulkan(vkCreateDevice(m_pDevice, &deviceInfo, nullptr, &m_device), "Create device")) {
        return false;
    }
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_gfxQueueFamIdx, graphicsQueueIndex, &m_gfxQueue);
    vkGetDeviceQueue(m_device, m_transferQueueFamIdx, graphicsQueueIndex, &m_transferQueue);
    if (m_gfxQueue == VK_NULL_HANDLE || m_transferQueue == VK_NULL_HANDLE) {
        return false;
    }
    m_pipelineCachePath = userDataDirectory("Zenith", "Zenith") / "pipeline-cache.bin";
    const std::vector<uint8_t> cacheBytes = readPipelineCache(m_pipelineCachePath);
    VkPipelineCacheCreateInfo cacheInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                        .initialDataSize = cacheBytes.size(),
                                        .pInitialData = cacheBytes.empty() ? nullptr : cacheBytes.data()};
    VkResult cacheResult = vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache);
    m_pipelineCacheWarm = cacheResult == VK_SUCCESS && !cacheBytes.empty();
    if (cacheResult != VK_SUCCESS && !cacheBytes.empty()) {
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        cacheResult = vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache);
        m_pipelineCacheWarm = false;
    }
    return checkVulkan(cacheResult, "Create pipeline cache");
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
    if (m_device && m_pipelineCache) {
        size_t cacheSize = 0;
        if (vkGetPipelineCacheData(m_device, m_pipelineCache, &cacheSize, nullptr) == VK_SUCCESS && cacheSize > 0) {
            std::vector<uint8_t> cacheBytes(cacheSize);
            if (vkGetPipelineCacheData(m_device, m_pipelineCache, &cacheSize, cacheBytes.data()) == VK_SUCCESS) {
                cacheBytes.resize(cacheSize);
                const PlatformResult saved = atomicWriteUserData(m_pipelineCachePath, cacheBytes);
                if (!saved) {
                    std::cerr << "Cannot persist pipeline cache: " << saved.error << '\n';
                }
            }
        }
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    }
    m_pipelineCache = VK_NULL_HANDLE;
    m_pipelineCacheWarm = false;
    m_pipelineCachePath.clear();
    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_gfxQueue = VK_NULL_HANDLE;
    m_transferQueue = VK_NULL_HANDLE;
    m_timelineSemaphoreSupported = false;
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
