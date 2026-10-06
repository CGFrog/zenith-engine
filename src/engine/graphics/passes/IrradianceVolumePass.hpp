#pragma once

// Owns the device-local probe buffer behind camera-set binding 8, which mesh.frag reads as IrradianceVolumeProbes
// (irradiance_volume::GpuProbe records).

#include "../vulkan/VulkanContext.hpp"
#include "engine/assets/IrradianceVolumeAsset.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <memory>
#include <span>
#include <vector>

// Owns the device-local probe buffer behind camera-set binding 8, which
// mesh.frag reads as IrradianceVolumeProbes (irradiance_volume::GpuProbe
// records). One buffer is shared by every frame in flight: an upload is
// recorded in the frame's command buffer between barriers that order it after
// earlier frames' fragment reads and before this frame's, and it happens only
// when the bound volume's data changes. Each frame slot's camera sets are
// rewritten lazily when the buffer is reallocated, once that slot's previous
// submission retired; replaced buffers are destroyed a full ring later.
class IrradianceVolumePass {
  public:
    static constexpr uint32_t binding = 8;

    bool init(const VulkanContext& context, uint32_t frameCount);
    void shutdown();

    // Records an upload of `data` when it differs from the buffer contents and
    // points this slot's camera sets at the current buffer. A null `data`
    // leaves the buffer as is. Must be recorded outside dynamic rendering.
    // frameSerial increases by one per recorded frame.
    bool prepare(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint64_t frameSerial,
                 const std::shared_ptr<const IrradianceVolumeData>& data, std::span<const VkDescriptorSet> cameraSets);

    // Points every slot's camera sets at the current buffer; call once after
    // the sets are allocated so binding 8 is never left unwritten.
    void writeDescriptors(uint32_t frameIndex, std::span<const VkDescriptorSet> cameraSets);

    uint64_t allocatedBytes() const {
        return m_capacity;
    }

    // The current probe buffer (never null after init). Uploads are visible to
    // fragment and compute shaders recorded after prepare().
    VkBuffer buffer() const {
        return m_buffer.buffer;
    }

  private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    bool allocate(VkDeviceSize size, Buffer& output) const;
    void destroy(const Buffer& buffer) const;
    void recordUpload(VkCommandBuffer commandBuffer, std::span<const std::byte> bytes) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    Buffer m_buffer;
    VkDeviceSize m_capacity{0};
    // Buffer each frame slot's camera sets currently reference.
    std::vector<VkBuffer> m_boundBuffers;
    std::shared_ptr<const IrradianceVolumeData> m_uploaded;
    // Packed probe records the buffer holds; empty until the first upload.
    std::vector<std::byte> m_uploadedBytes;
    DeferredReleaseQueue m_releases;
};
