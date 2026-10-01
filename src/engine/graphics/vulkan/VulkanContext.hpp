#pragma once
#include "../surface/Surface.hpp"
#include <filesystem>
#include <vk_mem_alloc.h>
#include <volk.h>

class VulkanContext {
  public:
    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool initInstance();
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

  private:
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
    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    std::filesystem::path m_pipelineCachePath;
    bool m_pipelineCacheWarm{false};
};
