#pragma once

// Host-visible copy of one id-buffer region: R32_UINT object ids followed by R32_SFLOAT depth, each tightly packed row
// by row.

#include "../vulkan/VulkanContext.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <volk.h>

// Host-visible copy of one id-buffer region: R32_UINT object ids followed by
// R32_SFLOAT depth, each tightly packed row by row. Unlike ImageReadback it
// never waits: the owner records the copy into a frame, keeps the readback
// until that frame's fence signalled, and only then reads it.
//
// Usage: prepare -> recordCopy (inside the frame) -> [frame fence signals] ->
// read -> release (or destruction). Owns its buffer until release.
class IdReadback {
  public:
    IdReadback() = default;
    IdReadback(const IdReadback&) = delete;
    IdReadback& operator=(const IdReadback&) = delete;
    ~IdReadback();

    // Empty on success, otherwise why the buffer could not be allocated.
    std::string prepare(VmaAllocator allocator, uint32_t width, uint32_t height);
    // Copies the top-left width x height texels of both images, which must be
    // in TRANSFER_SRC_OPTIMAL with their writes made available to transfer,
    // and makes the buffer visible to host reads after the frame completes.
    void recordCopy(VkCommandBuffer commandBuffer, VkImage idImage, VkImage depthImage) const;
    // Call only after the recording frame completed. Empty on success.
    std::string read(std::vector<uint32_t>& ids, std::vector<float>& depth) const;
    void release();

    uint32_t width() const {
        return m_width;
    }

    uint32_t height() const {
        return m_height;
    }

  private:
    VkDeviceSize planeBytes() const {
        return static_cast<VkDeviceSize>(m_width) * m_height * sizeof(uint32_t);
    }

    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkBuffer m_buffer{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    uint32_t m_width{0};
    uint32_t m_height{0};
};
