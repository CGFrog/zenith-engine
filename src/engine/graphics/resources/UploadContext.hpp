#pragma once

#include <functional>
#include <vk_mem_alloc.h>
#include <volk.h>

class VulkanContext;

// Reuses one command buffer and fence for infrequent resource uploads. The
// caller owns staging resources until submit() returns.
class UploadContext {
  public:
    bool init(const VulkanContext& context);
    VkBuffer stage(const void* data, VkDeviceSize size);
    bool submit(const std::function<void(VkCommandBuffer)>& record);

    double lastGpuMilliseconds() const {
        return m_lastGpuMilliseconds;
    }

    void shutdown();

    bool timingsSupported() const {
        return m_timestampQueries != VK_NULL_HANDLE;
    }

    uint64_t takeStagedBytes() {
        const uint64_t result = m_stagedBytes;
        m_stagedBytes = 0;
        return result;
    }

  private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    VkCommandPool m_pool{VK_NULL_HANDLE};
    VkCommandBuffer m_command{VK_NULL_HANDLE};
    VkFence m_fence{VK_NULL_HANDLE};
    VkQueryPool m_timestampQueries{VK_NULL_HANDLE};
    float m_timestampPeriod{0.0f};
    uint32_t m_timestampValidBits{0};
    uint64_t m_stagedBytes{0};
    double m_lastGpuMilliseconds{0.0};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkBuffer m_stagingBuffer{VK_NULL_HANDLE};
    VmaAllocation m_stagingAllocation{VK_NULL_HANDLE};
    void* m_stagingMapped{nullptr};
    VkDeviceSize m_stagingCapacity{0};
};
