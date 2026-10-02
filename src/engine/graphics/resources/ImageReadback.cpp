#include "ImageReadback.hpp"

#include <string>

namespace {
constexpr uint32_t texelBytes = 4;

enum class ChannelOrder {
    Bgra,
    Rgba,
};

bool channelOrder(VkFormat format, ChannelOrder& order) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
        order = ChannelOrder::Bgra;
        return true;
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
        order = ChannelOrder::Rgba;
        return true;
    default:
        return false;
    }
}

constexpr VkImageSubresourceRange colorRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
} // namespace

ImageFileResult convertReadbackToRgba(std::span<const uint8_t> texels, uint32_t width, uint32_t height, VkFormat format,
                                      RgbaImage& output) {
    ChannelOrder order{};
    if (!channelOrder(format, order)) {
        return {.error = "Frame capture does not support swapchain format " + std::to_string(format)};
    }
    const size_t byteCount = static_cast<size_t>(width) * height * texelBytes;
    if (width == 0 || height == 0 || texels.size() != byteCount) {
        return {.error = "Frame capture readback size does not match the image extent"};
    }
    output.width = width;
    output.height = height;
    output.pixels.resize(byteCount);
    const uint32_t red = order == ChannelOrder::Bgra ? 2 : 0;
    const uint32_t blue = order == ChannelOrder::Bgra ? 0 : 2;
    for (size_t base = 0; base < byteCount; base += texelBytes) {
        output.pixels[base + 0] = texels[base + red];
        output.pixels[base + 1] = texels[base + 1];
        output.pixels[base + 2] = texels[base + blue];
        output.pixels[base + 3] = 255;
    }
    return {};
}

ImageReadback::~ImageReadback() {
    release();
}

ImageFileResult ImageReadback::prepare(const VulkanContext& context, VkExtent2D extent, VkFormat format) {
    release();
    ChannelOrder order{};
    if (!channelOrder(format, order)) {
        return {.error = "Frame capture does not support swapchain format " + std::to_string(format)};
    }
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = static_cast<VkDeviceSize>(extent.width) * extent.height * texelBytes,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO};
    const VkResult result =
        vmaCreateBuffer(context.vmaAllocator(), &buffer, &allocation, &m_buffer, &m_allocation, nullptr);
    if (result != VK_SUCCESS) {
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        return {.error = "Cannot allocate the frame capture buffer (VkResult " + std::to_string(result) + ")"};
    }
    m_allocator = context.vmaAllocator();
    m_extent = extent;
    m_format = format;
    return {};
}

void ImageReadback::record(VkCommandBuffer commandBuffer, VkImage image) const {
    // The final pass's own barrier into PRESENT_SRC_KHR waits on nothing, so
    // this one waits on all prior commands; layout transitions on one queue
    // already execute in submission order.
    const VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = image,
                                           .subresourceRange = colorRange};
    const VkDependencyInfo before{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toTransfer};
    vkCmdPipelineBarrier2(commandBuffer, &before);

    const VkBufferImageCopy region{.bufferOffset = 0,
                                   .bufferRowLength = 0,
                                   .bufferImageHeight = 0,
                                   .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .mipLevel = 0,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1},
                                   .imageOffset = {0, 0, 0},
                                   .imageExtent = {m_extent.width, m_extent.height, 1}};
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &region);

    // Presentation waits on the frame's semaphore, so the image needs no
    // destination stage; the buffer becomes visible to host reads.
    const VkImageMemoryBarrier2 toPresent{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                          .srcAccessMask = 0,
                                          .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
                                          .dstAccessMask = 0,
                                          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = image,
                                          .subresourceRange = colorRange};
    const VkBufferMemoryBarrier2 toHost{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = m_buffer,
                                        .offset = 0,
                                        .size = VK_WHOLE_SIZE};
    const VkDependencyInfo after{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .bufferMemoryBarrierCount = 1,
                                 .pBufferMemoryBarriers = &toHost,
                                 .imageMemoryBarrierCount = 1,
                                 .pImageMemoryBarriers = &toPresent};
    vkCmdPipelineBarrier2(commandBuffer, &after);
}

ImageFileResult ImageReadback::read(RgbaImage& output) const {
    if (!m_buffer) {
        return {.error = "Frame capture buffer was not prepared"};
    }
    // Invalidation is a no-op for host-coherent memory and required otherwise.
    if (const VkResult result = vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
        result != VK_SUCCESS) {
        return {.error = "Cannot invalidate the frame capture buffer (VkResult " + std::to_string(result) + ")"};
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, m_allocation, &info);
    const size_t byteCount = static_cast<size_t>(m_extent.width) * m_extent.height * texelBytes;
    const std::span texels(static_cast<const uint8_t*>(info.pMappedData), byteCount);
    return convertReadbackToRgba(texels, m_extent.width, m_extent.height, m_format, output);
}

void ImageReadback::release() {
    if (m_buffer) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
