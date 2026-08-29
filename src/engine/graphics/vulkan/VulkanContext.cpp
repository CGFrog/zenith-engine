#include "VulkanContext.hpp"


VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT msgSeverity, VkDebugUtilsMessageTypeFlagsEXT msgType, const VkDebugUtilsMessengerCallbackDataEXT * pCallbackData, void* pUserData){
    if (msgSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){
        std::cerr << pCallbackData->pMessage << std::endl;
    }
    return VK_FALSE;
}

VkInstance VulkanContext::instance() const{
    return m_instance;
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
    return true;
}

bool VulkanContext::initPhysicalDevice(const Surface& surface ){
    uint32_t pDeviceCount{0};
    vkEnumeratePhysicalDevices(m_instance, &pDeviceCount, nullptr);
    std::vector<VkPhysicalDevice> pDevices(pDeviceCount);
    vkEnumeratePhysicalDevices(m_instance, &pDeviceCount, pDevices.data());

    m_pDevice = nullptr;
	if (pDeviceCount)
	{
		m_pDevice = pDevices[0];
        //  Eventually ill need to make this a bit more robust and pick the best quality GPU but this works for now.
		for (auto &pDev : pDevices)
		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(pDev, &props);
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				m_pDevice = pDev;
                return true;
            }
		}
	}
	return false;
}

VkPhysicalDevice VulkanContext::pDevice() const{
    return m_pDevice;
}

// borrowed from https://github.com/nickenchev/modern-vulkan/blob/lesson_01/src/application.cpp with adjustments
bool VulkanContext::initGfxQueue(const Surface& surface)
{
	uint32_t queueFamCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties2(m_pDevice, &queueFamCount, nullptr);
	std::vector<VkQueueFamilyProperties2> queueFamProps(queueFamCount, { VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 });
	vkGetPhysicalDeviceQueueFamilyProperties2(m_pDevice, &queueFamCount, queueFamProps.data());

	for (int currentFamIdx = 0; currentFamIdx < queueFamProps.size(); currentFamIdx++) {
		VkBool32 hasPresentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(m_pDevice, currentFamIdx, surface.handle(), &hasPresentSupport);

		const auto &props = queueFamProps[currentFamIdx];
		if (props.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT && hasPresentSupport) {
			m_gfxQueueFamIdx = currentFamIdx;
			return true;
		}
	}
	return false;
}

bool VulkanContext::initDevice() {
	float queuePriority{1.0f};
    if (m_gfxQueueFamIdx == UINT32_MAX){
        return false;
    }
	std::vector<uint32_t> queueFamiles{ m_gfxQueueFamIdx };

	VkDeviceQueueCreateInfo gfxQueueInfo {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = m_gfxQueueFamIdx,
		.queueCount = 1,
		.pQueuePriorities = &queuePriority
	};

	VkPhysicalDeviceVulkan14Features supportedFeatures14{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr };
	VkPhysicalDeviceVulkan13Features supportedFeatures13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14 };
	VkPhysicalDeviceVulkan12Features supportedFeatures12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13 };
	VkPhysicalDeviceFeatures2 supportedFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures12 };
	vkGetPhysicalDeviceFeatures2(m_pDevice, &supportedFeatures);

	if (!supportedFeatures13.dynamicRendering || !supportedFeatures13.synchronization2 || !supportedFeatures12.timelineSemaphore) {
		return false;
	}

	// produce a separate features struct chain for device creation
	VkPhysicalDeviceVulkan14Features features14 {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
		.pNext = nullptr,
	};
	VkPhysicalDeviceVulkan13Features features13 {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &features14,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE,
	};
	VkPhysicalDeviceVulkan12Features features12 {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.timelineSemaphore = VK_TRUE
	};
	VkPhysicalDeviceFeatures2 features{ 
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 
        .pNext = &features12 
    };

	const std::vector<const char *> deviceExtensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo devCreateInfo {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &gfxQueueInfo,
		.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
		.ppEnabledExtensionNames = deviceExtensions.data(),
		.pEnabledFeatures = nullptr
	};

	if (vkCreateDevice(m_pDevice, &devCreateInfo, nullptr, &m_device) != VK_SUCCESS) {
		return false;
	}

	vkGetDeviceQueue(m_device, m_gfxQueueFamIdx, 0, &m_gfxQueue);
	return m_gfxQueue;
}

bool VulkanContext::initVMA() {
	VmaVulkanFunctions vmaFuncInfo{};
	VmaAllocatorCreateInfo vmaAllocInfo {
		.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		.physicalDevice = m_pDevice,
		.device = m_device,
		.pVulkanFunctions = &vmaFuncInfo,
		.instance = m_instance,
		.vulkanApiVersion = VulkanVersion
	};

	vmaImportVulkanFunctionsFromVolk(&vmaAllocInfo, &vmaFuncInfo);
	return vmaCreateAllocator(&vmaAllocInfo, &vmaAllocator) == VK_SUCCESS;
}

void VulkanContext::closeInstance() {
    if (m_instance){
        vkDestroyInstance(m_instance, nullptr);
    }
    volkFinalize();
}