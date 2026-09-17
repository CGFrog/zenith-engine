#pragma once
#include "../surface/Surface.hpp"
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

    uint32_t gfxQueueIndex() const {
        return m_gfxQueueFamIdx;
    }

    VmaAllocator vmaAllocator() const {
        return m_vmaAllocator;
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
    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
};
