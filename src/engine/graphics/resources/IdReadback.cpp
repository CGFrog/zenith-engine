#include "IdReadback.hpp"

#include <cstring>

IdReadback::~IdReadback() {
    release();
}

std::string IdReadback::prepare(VmaAllocator allocator, uint32_t width, uint32_t height) {
    release();
    if (width == 0 || height == 0) {
        return "An id readback needs a non-empty region";
    }
    m_width = width;
    m_height = height;
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = planeBytes() * 2,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO};
    const VkResult result = vmaCreateBuffer(allocator, &buffer, &allocation, &m_buffer, &m_allocation, nullptr);
    if (result != VK_SUCCESS) {
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_width = 0;
        m_height = 0;
        return "Cannot allocate the pick readback buffer (VkResult " + std::to_string(result) + ")";
    }
    m_allocator = allocator;
    return {};
}

void IdReadback::recordCopy(VkCommandBuffer commandBuffer, VkImage idImage, VkImage depthImage) const {
    const auto region = [this](VkDeviceSize offset) {
        return VkBufferImageCopy{.bufferOffset = offset,
                                 .bufferRowLength = 0,
                                 .bufferImageHeight = 0,
                                 .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                      .mipLevel = 0,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1},
                                 .imageOffset = {0, 0, 0},
                                 .imageExtent = {m_width, m_height, 1}};
    };
    const VkBufferImageCopy idRegion = region(0);
    const VkBufferImageCopy depthRegion = region(planeBytes());
    vkCmdCopyImageToBuffer(commandBuffer, idImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &idRegion);
    vkCmdCopyImageToBuffer(commandBuffer, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &depthRegion);
    // The frame's fence makes the copy available; this makes it visible to
    // host reads once that fence signalled.
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
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &toHost};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

std::string IdReadback::read(std::vector<uint32_t>& ids, std::vector<float>& depth) const {
    if (!m_buffer) {
        return "The pick readback buffer was not prepared";
    }
    // Invalidation is a no-op for host-coherent memory and required otherwise.
    if (const VkResult result = vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
        result != VK_SUCCESS) {
        return "Cannot invalidate the pick readback buffer (VkResult " + std::to_string(result) + ")";
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, m_allocation, &info);
    const auto* bytes = static_cast<const std::byte*>(info.pMappedData);
    const size_t texels = static_cast<size_t>(m_width) * m_height;
    ids.resize(texels);
    depth.resize(texels);
    std::memcpy(ids.data(), bytes, texels * sizeof(uint32_t));
    std::memcpy(depth.data(), bytes + planeBytes(), texels * sizeof(float));
    return {};
}

void IdReadback::release() {
    if (m_buffer) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_width = 0;
    m_height = 0;
}
