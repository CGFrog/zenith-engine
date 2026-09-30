#pragma once
#include <volk.h>

struct GpuFrameTimings {
    bool valid{false};
    uint64_t frame{0};
    double frameMilliseconds{0.0};
    double shadowMilliseconds{0.0};
    double opaqueMilliseconds{0.0};
    double blendedMilliseconds{0.0};
    double sceneMilliseconds{0.0};
    double postMilliseconds{0.0};
    double bloomMilliseconds{0.0};
    double uiMilliseconds{0.0};
};

// Owns the resources reused for one frame of CPU work.
// Call shutdown after GPU work finishes, before destroying the device.
class FrameResources {
  public:
    FrameResources() = default;
    FrameResources(const FrameResources&) = delete;
    FrameResources& operator=(const FrameResources&) = delete;

    bool init(VkDevice device, uint32_t queueFamily, float timestampPeriod, uint32_t timestampValidBits);
    void shutdown(VkDevice device);

    bool waitUntilAvailable(VkDevice device) const;
    bool beginRecording() const;
    bool endRecording() const;
    bool submit(VkDevice device, VkQueue queue, VkSemaphore renderComplete) const;
    bool collectTimings(VkDevice device, GpuFrameTimings& timings);
    void writeTimestamp(uint32_t query, VkPipelineStageFlags2 stage) const;

    void markSubmitted(uint64_t frame, bool bloomEnabled) {
        m_sampleFrame = frame;
        m_bloomEnabled = bloomEnabled;
        m_timestampsPending = m_timestampQueries != VK_NULL_HANDLE;
    }

    VkQueryPool timestampQueries() const {
        return m_timestampQueries;
    }

    VkCommandBuffer commandBuffer() const {
        return m_commandBuffer;
    }

    VkSemaphore imageAcquired() const {
        return m_imageAcquired;
    }

  private:
    bool createCommandResources(VkDevice device, uint32_t queueFamily);
    bool createSynchronization(VkDevice device);
    bool createTimestampQueries(VkDevice device);

    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};
    VkSemaphore m_imageAcquired{VK_NULL_HANDLE};
    VkFence m_fence{VK_NULL_HANDLE};
    VkQueryPool m_timestampQueries{VK_NULL_HANDLE};
    float m_timestampPeriod{0.0f};
    bool m_timestampsPending{false};
    bool m_bloomEnabled{false};
    uint32_t m_timestampValidBits{0};
    uint64_t m_sampleFrame{0};
};
