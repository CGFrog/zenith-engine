#pragma once

#include "../../diagnostics/RgbaImage.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <span>
#include <string>
#include <volk.h>

// Converts tightly packed 8-bit color texels read back from an image of
// `format` into RGBA. Supports the 8-bit BGRA and RGBA swapchain formats (UNORM
// and SRGB; encoded values are kept as-is) and writes opaque alpha. Returns an
// error for any other format or a size mismatch.
ImageFileResult convertReadbackToRgba(std::span<const uint8_t> texels, uint32_t width, uint32_t height, VkFormat format,
                                      RgbaImage& output);

// Copies one color image to host memory. Usage per capture:
//   prepare -> record (inside the frame's command buffer) -> submit and wait for
//   that submission -> read -> release.
// The image must be in PRESENT_SRC_KHR when the recorded commands run (after
// the frame's final pass) and is returned to PRESENT_SRC_KHR; it must have been
// created with TRANSFER_SRC usage. Owns its staging buffer until release.
class ImageReadback {
  public:
    ImageReadback() = default;
    ImageReadback(const ImageReadback&) = delete;
    ImageReadback& operator=(const ImageReadback&) = delete;
    ~ImageReadback();

    ImageFileResult prepare(const VulkanContext& context, VkExtent2D extent, VkFormat format);
    void record(VkCommandBuffer commandBuffer, VkImage image) const;
    ImageFileResult read(RgbaImage& output) const;
    void release();

  private:
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkBuffer m_buffer{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    VkFormat m_format{VK_FORMAT_UNDEFINED};
};
