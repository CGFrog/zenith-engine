#pragma once
#include "../GraphicsValidation.hpp"
#include "../surface/Surface.hpp"
#include <atomic>
#include <filesystem>
#include <vk_mem_alloc.h>
#include <volk.h>

class VulkanContext {
  public:
    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Strict validation requires the validation layer and enables
    // synchronization validation; initInstance fails when either is missing.
    bool initInstance(const GraphicsValidationOptions& validation = {});
    bool initPhysicalDevice(const Surface& surface);
    bool initGfxQueue(const Surface& surface);
    bool initDevice();
    bool initVMA();
    void closeInstance();
    void closeDevice();

    VkInstance instance() const {
        return m_instance;
    }

    VkPhysicalDevice pDevice() const {
        return m_pDevice;
    }

    VkDevice device() const {
        return m_device;
    }

    VkQueue gfxQueue() const {
        return m_gfxQueue;
    }

    VkQueue transferQueue() const {
        return m_transferQueue;
    }

    uint32_t transferQueueIndex() const {
        return m_transferQueueFamIdx;
    }

    bool hasDedicatedTransferQueue() const {
        return m_transferQueueFamIdx != m_gfxQueueFamIdx;
    }

    bool timelineSemaphoreSupported() const {
        return m_timelineSemaphoreSupported;
    }

    // True when queries can be reset from the host (vkResetQueryPool), which
    // transfer-only queues need because they cannot record vkCmdResetQueryPool.
    bool hostQueryResetSupported() const {
        return m_hostQueryResetSupported;
    }

    // True when BC1-BC7 images can be sampled (textureCompressionBC enabled).
    bool blockCompressionSupported() const {
        return m_blockCompressionSupported;
    }

    uint32_t gfxQueueIndex() const {
        return m_gfxQueueFamIdx;
    }

    VmaAllocator vmaAllocator() const {
        return m_vmaAllocator;
    }

    VkPipelineCache pipelineCache() const {
        return m_pipelineCache;
    }

    bool pipelineCacheWarm() const {
        return m_pipelineCacheWarm;
    }

    // Snapshot of the messages the validation layer reported so far. Remains
    // valid after closeInstance so callers can inspect teardown messages.
    GraphicsValidationReport validationReport() const;

  private:
    // Written by the debug messenger callback through pUserData; callbacks may
    // arrive on any thread that calls into Vulkan.
    struct ValidationCounters {
        std::atomic<uint32_t> errors{0};
        std::atomic<uint32_t> warnings{0};
        std::atomic<uint32_t> performanceWarnings{0};
        std::atomic<uint32_t> loaderMessages{0};
    };

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* message,
                                                        void* counters);
    bool supportsDevice(VkPhysicalDevice device, const Surface& surface) const;

    static constexpr uint32_t VulkanVersion = VK_API_VERSION_1_4;
    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkPhysicalDevice m_pDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    uint32_t m_gfxQueueFamIdx{UINT32_MAX};
    VkQueue m_gfxQueue{VK_NULL_HANDLE};
    uint32_t m_transferQueueFamIdx{UINT32_MAX};
    VkQueue m_transferQueue{VK_NULL_HANDLE};
    bool m_timelineSemaphoreSupported{false};
    bool m_hostQueryResetSupported{false};
    bool m_blockCompressionSupported{false};
    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    std::filesystem::path m_pipelineCachePath;
    bool m_pipelineCacheWarm{false};
    bool m_validationStrict{false};
    bool m_validationLayerActive{false};
    bool m_synchronizationValidation{false};
    ValidationCounters m_validationCounters;
};
