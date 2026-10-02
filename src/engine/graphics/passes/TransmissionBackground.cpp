#include "TransmissionBackground.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace {
VkImageSubresourceRange colorLevels(uint32_t baseLevel, uint32_t levelCount) {
    return {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = baseLevel,
            .levelCount = levelCount,
            .baseArrayLayer = 0,
            .layerCount = 1};
}

VkImageMemoryBarrier2 layoutBarrier(VkImage image, VkImageSubresourceRange range, VkImageLayout oldLayout,
                                    VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                    VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                                    VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStage,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStage,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range};
}

void pipelineBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2* barriers, uint32_t count) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = count, .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkOffset3D farCorner(VkExtent2D extent) {
    return {static_cast<int32_t>(std::max(extent.width, 1u)), static_cast<int32_t>(std::max(extent.height, 1u)), 1};
}

VkExtent2D levelExtent(VkExtent2D extent, uint32_t level) {
    return {std::max(extent.width >> level, 1u), std::max(extent.height >> level, 1u)};
}

void blit(VkCommandBuffer commandBuffer, VkImage source, uint32_t sourceLevel, VkExtent2D sourceExtent,
          VkImage destination, uint32_t destinationLevel, VkExtent2D destinationExtent) {
    const VkImageBlit region{
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = sourceLevel, .layerCount = 1},
        .srcOffsets = {{0, 0, 0}, farCorner(sourceExtent)},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = destinationLevel, .layerCount = 1},
        .dstOffsets = {{0, 0, 0}, farCorner(destinationExtent)}};
    vkCmdBlitImage(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
}
} // namespace

bool TransmissionBackground::init(const VulkanContext& context, VkExtent2D storageExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = VK_LOD_CLAMP_NONE};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create transmission sampler") ||
        !createImage(storageExtent)) {
        shutdown();
        return false;
    }
    return true;
}

bool TransmissionBackground::createImage(VkExtent2D storageExtent) {
    m_storageExtent = {std::max(storageExtent.width, 1u), std::max(storageExtent.height, 1u)};
    m_layoutInitialized = false;
    m_levels = static_cast<uint32_t>(std::bit_width(std::max(m_storageExtent.width, m_storageExtent.height)));
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {m_storageExtent.width, m_storageExtent.height, 1},
                                  .mipLevels = m_levels,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_image, &m_allocation, nullptr),
                     "Create transmission background")) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = m_image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = colorLevels(0, m_levels)};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_view), "Create transmission background view");
}

bool TransmissionBackground::resize(VkExtent2D storageExtent) {
    destroyImage();
    return createImage(storageExtent);
}

void TransmissionBackground::prepareFirstUse(VkCommandBuffer commandBuffer) {
    if (m_layoutInitialized || !m_image) {
        return;
    }
    // Never captured yet: transparent black, so a shader reading it before
    // the first capture sees nothing rather than undefined memory.
    const VkImageMemoryBarrier2 toClear = layoutBarrier(
        m_image, colorLevels(0, m_levels), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    pipelineBarrier(commandBuffer, &toClear, 1);
    const VkClearColorValue black{};
    const VkImageSubresourceRange levels = colorLevels(0, m_levels);
    vkCmdClearColorImage(commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &levels);
    const VkImageMemoryBarrier2 toRead =
        layoutBarrier(m_image, colorLevels(0, m_levels), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    pipelineBarrier(commandBuffer, &toRead, 1);
    m_layoutInitialized = true;
}

void TransmissionBackground::capture(VkCommandBuffer commandBuffer, VkImage sceneImage, VkExtent2D renderExtent) {
    prepareFirstUse(commandBuffer);
    // The scene's attachment writes finish before the copy reads them, and
    // earlier transmissive draws (this or a previous frame) finish sampling
    // before any level is overwritten.
    const std::array<VkImageMemoryBarrier2, 2> begin{
        layoutBarrier(sceneImage, colorLevels(0, 1), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT),
        layoutBarrier(m_image, colorLevels(0, m_levels), readLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT)};
    pipelineBarrier(commandBuffer, begin.data(), static_cast<uint32_t>(begin.size()));
    blit(commandBuffer, sceneImage, 0, renderExtent, m_image, 0, m_storageExtent);
    for (uint32_t level = 1; level < m_levels; ++level) {
        const VkImageMemoryBarrier2 source =
            layoutBarrier(m_image, colorLevels(level - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        pipelineBarrier(commandBuffer, &source, 1);
        blit(commandBuffer, m_image, level - 1, levelExtent(m_storageExtent, level - 1), m_image, level,
             levelExtent(m_storageExtent, level));
    }
    // Every level but the last was a blit source; the last is still a
    // destination.
    const uint32_t lastLevel = m_levels - 1;
    std::array<VkImageMemoryBarrier2, 3> end{
        layoutBarrier(m_image, colorLevels(lastLevel, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        layoutBarrier(sceneImage, colorLevels(0, 1), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        layoutBarrier(m_image, colorLevels(0, lastLevel), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readLayout,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)};
    // A single-level image has no source levels to release.
    pipelineBarrier(commandBuffer, end.data(), lastLevel > 0 ? 3u : 2u);
}

void TransmissionBackground::destroyImage() {
    if (m_view) {
        vkDestroyImageView(m_device, m_view, nullptr);
    }
    if (m_image) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_levels = 0;
    m_layoutInitialized = false;
}

void TransmissionBackground::shutdown() {
    if (!m_device) {
        return;
    }
    destroyImage();
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}
