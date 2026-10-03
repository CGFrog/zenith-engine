#include "IdBufferPass.hpp"

#include "../DepthConvention.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>

namespace {
bool hasStencil(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT;
}

VkImageAspectFlags depthAspects(VkFormat format) {
    return VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageAspectFlags aspects, VkImageLayout oldLayout,
                                   VkImageLayout newLayout, VkPipelineStageFlags2 sourceStages,
                                   VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStages,
                                   VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStages,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStages,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = aspects, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};
}

void pipelineBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2* barriers, uint32_t count) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = count, .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

bool IdBufferPass::init(const VulkanContext& context, VkPipelineCache pipelineCache, VkDescriptorSetLayout cameraLayout,
                        VkFormat depthFormat) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_depthFormat = depthFormat;
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaderRoot() / "id_buffer.vert.spv") || !fragment.load(shaderRoot() / "id_buffer.frag.spv")) {
        return false;
    }
    return m_pipeline.initIdBufferPipeline(m_device, idFormat, depthColorFormat, depthFormat, cameraLayout,
                                           vertex.handle(), fragment.handle(), pipelineCache, sizeof(IdBufferDraw));
}

bool IdBufferPass::begin(VkCommandBuffer commandBuffer, uint64_t frameSerial, VkExtent2D viewExtent,
                         const PickRect& region, VkDescriptorSet cameraSet) {
    if (region.width == 0 || region.height == 0 || !ensureTargets({region.width, region.height}, frameSerial)) {
        return false;
    }
    // Earlier regions (this frame or before) last copied the colour targets
    // and wrote the depth attachment; every target's contents are discarded.
    const VkImageMemoryBarrier2 toAttachment[]{
        imageBarrier(m_targets.idImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        imageBarrier(m_targets.depthColorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        imageBarrier(m_targets.depthImage, depthAspects(m_depthFormat), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};
    pipelineBarrier(commandBuffer, toAttachment, static_cast<uint32_t>(std::size(toAttachment)));

    const auto colorAttachment = [](VkImageView view) {
        return VkRenderingAttachmentInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = view,
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .clearValue = {.color = {.uint32 = {0, 0, 0, 0}}}};
    };
    const VkRenderingAttachmentInfo colors[]{colorAttachment(m_targets.idView),
                                             colorAttachment(m_targets.depthColorView)};
    const VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = m_targets.depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {.depth = scene_depth::clearValue, .stencil = 0}}};
    const VkExtent2D regionExtent{region.width, region.height};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = regionExtent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = static_cast<uint32_t>(std::size(colors)),
                                    .pColorAttachments = colors,
                                    .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
    // The whole view's viewport, shifted so the region's top-left pixel lands
    // on the targets' origin; the scissor keeps only the region.
    const VkViewport viewport{.x = -static_cast<float>(region.x),
                              .y = -static_cast<float>(region.y),
                              .width = static_cast<float>(viewExtent.width),
                              .height = static_cast<float>(viewExtent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    const VkRect2D scissor{.offset = {0, 0}, .extent = regionExtent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1, &cameraSet, 0,
                            nullptr);
    return true;
}

void IdBufferPass::draw(VkCommandBuffer commandBuffer, const IdBufferDraw& constants, uint32_t indexCount,
                        uint32_t firstIndex, int32_t vertexOffset) const {
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(constants), &constants);
    vkCmdDrawIndexed(commandBuffer, indexCount, 1, firstIndex, vertexOffset, 0);
}

void IdBufferPass::end(VkCommandBuffer commandBuffer, const IdReadback& readback) const {
    vkCmdEndRendering(commandBuffer);
    const VkImageMemoryBarrier2 toTransfer[]{
        imageBarrier(m_targets.idImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT),
        imageBarrier(m_targets.depthColorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT)};
    pipelineBarrier(commandBuffer, toTransfer, static_cast<uint32_t>(std::size(toTransfer)));
    readback.recordCopy(commandBuffer, m_targets.idImage, m_targets.depthColorImage);
}

void IdBufferPass::collect(uint64_t completedSerial) {
    m_releaseQueue.collect(completedSerial);
}

void IdBufferPass::shutdown() {
    m_releaseQueue.collect(UINT64_MAX);
    destroyTargets(m_targets);
    m_targets = {};
    if (m_device) {
        m_pipeline.shutdown(m_device);
    }
}

bool IdBufferPass::ensureTargets(VkExtent2D extent, uint64_t frameSerial) {
    if (m_targets.idImage && extent.width <= m_targets.extent.width && extent.height <= m_targets.extent.height) {
        return true;
    }
    const VkExtent2D grown{std::max(extent.width, m_targets.extent.width),
                           std::max(extent.height, m_targets.extent.height)};
    Targets replacement;
    if (!createTargets(grown, replacement)) {
        destroyTargets(replacement);
        return false;
    }
    // Commands recorded earlier in this frame may still use the old targets.
    if (m_targets.idImage) {
        m_releaseQueue.defer(frameSerial, [this, retired = m_targets] { destroyTargets(retired); });
    }
    m_targets = replacement;
    return true;
}

bool IdBufferPass::createTargets(VkExtent2D extent, Targets& targets) const {
    targets.extent = extent;
    constexpr VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return createImage(idFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT, extent, targets.idImage, targets.idAllocation,
                       targets.idView) &&
           createImage(depthColorFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT, extent, targets.depthColorImage,
                       targets.depthColorAllocation, targets.depthColorView) &&
           createImage(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, extent,
                       targets.depthImage, targets.depthAllocation, targets.depthView);
}

bool IdBufferPass::createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkExtent2D extent,
                               VkImage& image, VmaAllocation& allocation, VkImageView& view) const {
    const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = format,
                                      .extent = {extent.width, extent.height, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = usage,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &image, &allocation, nullptr),
                     "Create id-buffer image")) {
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return false;
    }
    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &view), "Create id-buffer image view");
}

void IdBufferPass::destroyTargets(const Targets& targets) const {
    const auto destroy = [this](VkImage image, VmaAllocation allocation, VkImageView view) {
        if (view) {
            vkDestroyImageView(m_device, view, nullptr);
        }
        if (image) {
            vmaDestroyImage(m_allocator, image, allocation);
        }
    };
    destroy(targets.idImage, targets.idAllocation, targets.idView);
    destroy(targets.depthColorImage, targets.depthColorAllocation, targets.depthColorView);
    destroy(targets.depthImage, targets.depthAllocation, targets.depthView);
}
