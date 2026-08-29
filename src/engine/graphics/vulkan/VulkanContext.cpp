#include "VulkanContext.hpp"


VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT msgSeverity, VkDebugUtilsMessageTypeFlagsEXT msgType, const VkDebugUtilsMessengerCallbackDataEXT * pCallbackData, void* pUserData){
    if (msgSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){
        std::cerr << pCallbackData->pMessage << std::endl;
    }
    return VK_FALSE;
}

bool VulkanContext::initInstance(){
    
    VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Zenith",
        .apiVersion = VulkanVersion
    };

    uint32_t instExtCnt{0};
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&instExtCnt);
    
    std::vector<const char *> requestedExtensions { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    requestedExtensions.resize(instExtCnt + 1);
    for (int i = 0; i < instExtCnt; ++i){
        requestedExtensions.emplace_back(extensions[i]);
    }

    std::vector <const char *> requestedLayers{
        "VK_LAYER_KHRONOS_validation"
    };

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugCallback
    };

    VkInstanceCreateInfo instCreateInfo {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &debugInfo,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t> (requestedLayers.size()),
        .ppEnabledLayerNames = requestedLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t> (requestedExtensions.size()),
        .ppEnabledExtensionNames = requestedExtensions.data()
    };

    if (vkCreateInstance(&instCreateInfo, nullptr, &m_instance)!= VK_SUCCESS){
        return false;
    }
    volkLoadInstance(m_instance);
}

void VulkanContext::closeInstance() {
    if (m_instance){
        vkDestroyInstance(m_instance, nullptr);
    }
    volkFinalize();
}