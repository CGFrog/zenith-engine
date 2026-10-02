#define VMA_IMPLEMENTATION
#include "VulkanContext.hpp"
#include "VulkanCheck.hpp"
#include "engine/platform/PlatformServices.hpp"
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_vulkan.h>
#include <cstring>
#include <fstream>

namespace {
constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t invalidQueueFamily = UINT32_MAX;
constexpr uint32_t graphicsQueueIndex = 0;
constexpr float graphicsQueuePriority = 1.0f;

bool validationLayerAvailable() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, validationLayer) == 0) {
            return true;
        }
    }
    return false;
}

bool layerProvidesExtension(const char* layer, const char* extension) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data());
    for (const VkExtensionProperties& candidate : extensions) {
        if (std::strcmp(candidate.extensionName, extension) == 0) {
            return true;
        }
    }
    return false;
}

const char* severityName(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    return severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
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
    VkPhysicalDeviceVulkan14Features vulkan14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan12Features indexingFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                      .pNext = &vulkan14};
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
    // Passes bind per-draw resources with push descriptors, write multiple
    // color attachments with different blend states, and shaders compiled for
    // SPIR-V 1.6 lower discard to demote-to-helper-invocation.
    const bool pipelineFeatures = vulkan14.pushDescriptor && features.features.independentBlend &&
                                  renderingFeatures.shaderDemoteToHelperInvocation;
    // Resident reflection probes are sampled from one cube-map array.
    return renderingFeatures.dynamicRendering && renderingFeatures.synchronization2 &&
           features.features.samplerAnisotropy && features.features.imageCubeArray && bindlessTextures &&
           pipelineFeatures;
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

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT types,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* message,
                                                            void* counters) {
    auto& validation = *static_cast<ValidationCounters*>(counters);
    const bool performance = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0;
    // The loader reports through the same messenger with this fixed ID, e.g.
    // for unreadable implicit-layer manifests left by uninstalled overlays.
    const bool loader =
        message->pMessageIdName != nullptr && std::strcmp(message->pMessageIdName, "Loader Message") == 0;
    if (loader) {
        validation.loaderMessages.fetch_add(1, std::memory_order_relaxed);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validation.errors.fetch_add(1, std::memory_order_relaxed);
    } else if (performance) {
        validation.performanceWarnings.fetch_add(1, std::memory_order_relaxed);
    } else {
        validation.warnings.fetch_add(1, std::memory_order_relaxed);
    }
    const char* messageId = message->pMessageIdName != nullptr ? message->pMessageIdName : "unnamed";
    std::cerr << "[Vulkan "
              << (loader        ? "loader "
                  : performance ? "performance "
                                : "")
              << severityName(severity) << "] " << messageId << ": " << message->pMessage << '\n';
    return VK_FALSE;
}

GraphicsValidationReport VulkanContext::validationReport() const {
    return {.strict = m_validationStrict,
            .layerActive = m_validationLayerActive,
            .synchronizationValidation = m_synchronizationValidation,
            .errorCount = m_validationCounters.errors.load(std::memory_order_relaxed),
            .warningCount = m_validationCounters.warnings.load(std::memory_order_relaxed),
            .performanceWarningCount = m_validationCounters.performanceWarnings.load(std::memory_order_relaxed),
            .loaderMessageCount = m_validationCounters.loaderMessages.load(std::memory_order_relaxed)};
}

bool VulkanContext::initInstance(const GraphicsValidationOptions& validation) {
    // Record strict mode before anything can fail so a missing loader or layer
    // still reports as a skipped strict run.
    m_validationStrict = validation.strict;
    if (!checkVulkan(volkInitialize(), "volkInitialize")) {
        return false;
    }
    std::vector<const char*> extensions;
    if (!requestedInstanceExtensions(extensions)) {
        return false;
    }
    std::vector<const char*> layers;
    m_validationLayerActive = validationLayerAvailable();
    if (m_validationLayerActive) {
        layers.push_back(validationLayer);
        std::cerr << "Vulkan validation enabled.\n";
    } else if (validation.strict) {
        std::cerr << "Strict validation requires " << validationLayer << ", which is not installed.\n";
        return false;
    } else {
        std::cerr << "Validation layer unavailable; continuing without validation.\n";
    }
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                 .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                                                 .pfnUserCallback = debugCallback,
                                                 .pUserData = &m_validationCounters};
    // Synchronization validation reports hazards that core validation cannot
    // see (missing or too-narrow barriers). It is slow, so only strict runs pay.
    const VkValidationFeatureEnableEXT synchronizationFeature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                               .pNext = &debugInfo,
                                               .enabledValidationFeatureCount = 1,
                                               .pEnabledValidationFeatures = &synchronizationFeature};
    m_synchronizationValidation = false;
    if (validation.strict) {
        if (!layerProvidesExtension(validationLayer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            std::cerr << "Strict validation requires " << VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME << ".\n";
            return false;
        }
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        m_synchronizationValidation = true;
    }
    VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Zenith", .apiVersion = VulkanVersion};
    VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pNext = m_synchronizationValidation
                                                   ? static_cast<const void*>(&validationFeatures)
                                                   : static_cast<const void*>(&debugInfo),
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
                     "synchronization2, push descriptor, independent blend, demote-to-helper, and bindless "
                     "texture support.\n";
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
    VkPhysicalDeviceVulkan14Features vulkan14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                                              .pushDescriptor = VK_TRUE};
    VkPhysicalDeviceFeatures2 supported{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported12};
    vkGetPhysicalDeviceFeatures2(m_pDevice, &supported);
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vulkan14,
        .drawIndirectCount = supported12.drawIndirectCount,
        .descriptorIndexing = supported12.descriptorIndexing,
        .shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing,
        .descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind,
        .descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound,
        .descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount,
        .runtimeDescriptorArray = supported12.runtimeDescriptorArray,
        .hostQueryReset = supported12.hostQueryReset,
        .timelineSemaphore = supported12.timelineSemaphore,
    };
    m_timelineSemaphoreSupported = supported12.timelineSemaphore == VK_TRUE;
    m_hostQueryResetSupported = supported12.hostQueryReset == VK_TRUE;
    VkPhysicalDeviceVulkan13Features vulkan13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                              .pNext = &vulkan12,
                                              .shaderDemoteToHelperInvocation = VK_TRUE,
                                              .synchronization2 = VK_TRUE,
                                              .dynamicRendering = VK_TRUE};
    // BC sampling is optional: without it, textures stay on the RGBA8 path.
    // ZENITH_DISABLE_BLOCK_COMPRESSION leaves it off on devices that have it,
    // so that path can be validated without such a device.
    const bool blockCompressionDisabled = SDL_getenv("ZENITH_DISABLE_BLOCK_COMPRESSION") != nullptr;
    m_blockCompressionSupported = supported.features.textureCompressionBC == VK_TRUE && !blockCompressionDisabled;
    if (blockCompressionDisabled) {
        std::cerr << "BC texture sampling disabled by ZENITH_DISABLE_BLOCK_COMPRESSION; textures use RGBA8\n";
    }
    // Optional: the single-pass depth-pyramid build indexes its level array.
    m_storageImageArrayDynamicIndexingSupported = supported.features.shaderStorageImageArrayDynamicIndexing == VK_TRUE;
    VkPhysicalDeviceFeatures enabledFeatures{.imageCubeArray = VK_TRUE,
                                             .independentBlend = VK_TRUE,
                                             .multiDrawIndirect = supported.features.multiDrawIndirect,
                                             .samplerAnisotropy = VK_TRUE,
                                             .textureCompressionBC = m_blockCompressionSupported ? VK_TRUE : VK_FALSE,
                                             .shaderStorageImageArrayDynamicIndexing =
                                                 supported.features.shaderStorageImageArrayDynamicIndexing};
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
    m_hostQueryResetSupported = false;
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
