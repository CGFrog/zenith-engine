#pragma once

#include <array>
#include <functional>
#include <span>
#include <vk_mem_alloc.h>
#include <volk.h>

class VulkanContext;

// Bounded staging ring. Timeline-capable uploads never wait for a busy slot;
// callers defer work when stage() reports backpressure.
class UploadContext {
  public:
    // Largest single stage() request; a larger upload can never succeed.
    static constexpr VkDeviceSize maximumSlotBytes = 64ull * 1024ull * 1024ull;

    bool init(const VulkanContext& context, bool preferDedicatedTransferQueue = true);
    VkBuffer stage(const void* data, VkDeviceSize size);
    bool submit(const std::function<void(VkCommandBuffer)>& record);
    void shutdown();

    double lastGpuMilliseconds() const {
        return m_lastGpuMilliseconds;
    }

    VkSemaphore timelineSemaphore() const {
        return m_timelineSemaphore;
    }

    uint64_t requiredTimelineValue() const {
        return m_lastSubmittedValue;
    }

    bool usesDedicatedQueue() const {
        return m_queueFamily != m_graphicsQueueFamily;
    }

    bool timingsSupported() const {
        return m_timestampQueries != VK_NULL_HANDLE;
    }

    std::span<const uint32_t> sharingQueueFamilies() const {
        return {m_sharingQueueFamilies.data(), m_sharingQueueFamilyCount};
    }

    uint64_t takeStagedBytes() {
        const uint64_t result = m_stagedBytes;
        m_stagedBytes = 0;
        return result;
    }

    uint32_t takeBackpressureEvents() {
        const uint32_t result = m_backpressureEvents;
        m_backpressureEvents = 0;
        return result;
    }

  private:
    static constexpr uint32_t ringSize = 3;

    struct Slot {
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        VkBuffer stagingBuffer{VK_NULL_HANDLE};
        VmaAllocation stagingAllocation{VK_NULL_HANDLE};
        void* stagingMapped{nullptr};
        VkDeviceSize stagingCapacity{0};
        uint64_t pendingValue{0};
    };

    bool retireCompleted(bool waitForAll);

    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    VkCommandPool m_pool{VK_NULL_HANDLE};
    VkSemaphore m_timelineSemaphore{VK_NULL_HANDLE};
    VkQueryPool m_timestampQueries{VK_NULL_HANDLE};
    float m_timestampPeriod{0.0f};
    uint32_t m_timestampValidBits{0};
    uint64_t m_stagedBytes{0};
    double m_lastGpuMilliseconds{0.0};
    uint64_t m_nextTimelineValue{1};
    uint64_t m_lastSubmittedValue{0};
    uint32_t m_nextSlot{0};
    uint32_t m_activeSlot{UINT32_MAX};
    uint32_t m_backpressureEvents{0};
    uint32_t m_queueFamily{UINT32_MAX};
    uint32_t m_graphicsQueueFamily{UINT32_MAX};
    std::array<uint32_t, 2> m_sharingQueueFamilies{};
    uint32_t m_sharingQueueFamilyCount{1};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    std::array<Slot, ringSize> m_slots{};
};
