#pragma once
#include <volk.h>

// Owns the resources reused for one frame of CPU work.
// Call shutdown after GPU work finishes, before destroying the device.
class FrameResources {
  public:
    FrameResources() = default;
    FrameResources(const FrameResources&) = delete;
    FrameResources& operator=(const FrameResources&) = delete;

    bool init(VkDevice device, uint32_t queueFamily);
    void shutdown(VkDevice device);

    bool waitUntilAvailable(VkDevice device) const;
    bool beginRecording() const;
    bool endRecording() const;
    bool submit(VkDevice device, VkQueue queue, VkSemaphore renderComplete) const;

    VkCommandBuffer commandBuffer() const {
        return m_commandBuffer;
    }

    VkSemaphore imageAcquired() const {
        return m_imageAcquired;
    }

  private:
    bool createCommandResources(VkDevice device, uint32_t queueFamily);
    bool createSynchronization(VkDevice device);

    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};
    VkSemaphore m_imageAcquired{VK_NULL_HANDLE};
    VkFence m_fence{VK_NULL_HANDLE};
};
