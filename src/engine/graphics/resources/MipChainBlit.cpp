#include "MipChainBlit.hpp"

#include <algorithm>
#include <bit>
#include <cassert>

namespace {
void transitionLevel(VkCommandBuffer commandBuffer, const MipChainBlitTarget& target, uint32_t mip,
                     VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                     VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                     VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStage,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStage,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = newLayout,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = target.image,
                                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                             .baseMipLevel = mip,
                                                             .levelCount = 1,
                                                             .baseArrayLayer = 0,
                                                             .layerCount = target.layerCount}};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

uint32_t fullMipChainLength(uint32_t width, uint32_t height) {
    const uint32_t largest = std::max(width, height);
    return largest == 0 ? 0u : static_cast<uint32_t>(std::bit_width(largest));
}

std::vector<MipBlitStep> planMipChainBlits(uint32_t width, uint32_t height, uint32_t mipCount) {
    const uint32_t levels = std::min(mipCount, fullMipChainLength(width, height));
    std::vector<MipBlitStep> steps;
    if (levels < 2) {
        return steps;
    }
    steps.reserve(levels - 1);
    int32_t sourceWidth = static_cast<int32_t>(width);
    int32_t sourceHeight = static_cast<int32_t>(height);
    for (uint32_t mip = 1; mip < levels; ++mip) {
        const int32_t destinationWidth = std::max(sourceWidth / 2, 1);
        const int32_t destinationHeight = std::max(sourceHeight / 2, 1);
        steps.push_back({.sourceMip = mip - 1,
                         .sourceWidth = sourceWidth,
                         .sourceHeight = sourceHeight,
                         .destinationWidth = destinationWidth,
                         .destinationHeight = destinationHeight});
        sourceWidth = destinationWidth;
        sourceHeight = destinationHeight;
    }
    return steps;
}

void recordMipChainBlits(VkCommandBuffer commandBuffer, const MipChainBlitTarget& target, MipChainConsumers consumers) {
    assert(target.mipCount <= fullMipChainLength(target.width, target.height));
    if (target.mipCount == 0) {
        return;
    }
    for (const MipBlitStep& step : planMipChainBlits(target.width, target.height, target.mipCount)) {
        transitionLevel(commandBuffer, target, step.sourceMip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkImageBlit blit{.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                  .mipLevel = step.sourceMip,
                                                  .baseArrayLayer = 0,
                                                  .layerCount = target.layerCount},
                               .srcOffsets = {{0, 0, 0}, {step.sourceWidth, step.sourceHeight, 1}},
                               .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                  .mipLevel = step.sourceMip + 1,
                                                  .baseArrayLayer = 0,
                                                  .layerCount = target.layerCount},
                               .dstOffsets = {{0, 0, 0}, {step.destinationWidth, step.destinationHeight, 1}}};
        vkCmdBlitImage(commandBuffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        transitionLevel(commandBuffer, target, step.sourceMip, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT, consumers.stages, consumers.access);
    }
    // The last level was only ever written, never read by a blit.
    transitionLevel(commandBuffer, target, target.mipCount - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, consumers.stages, consumers.access);
}
