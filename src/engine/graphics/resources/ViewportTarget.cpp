#include "ViewportTarget.hpp"

#include "../vulkan/VulkanCheck.hpp"

namespace {
bool createView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView& output) {
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};
    return checkVulkan(vkCreateImageView(device, &view, nullptr, &output), "Create viewport image view");
}
} // namespace

bool ViewportTarget::init(const VulkanContext& context, UiPass& uiPass, VkFormat colorFormat, VkFormat depthFormat,
                          uint32_t frameCount, VkExtent2D extent) {
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_colors.resize(frameCount);
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_LINEAR,
                                .minFilter = VK_FILTER_LINEAR,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(context.device(), &sampler, nullptr, &m_sampler), "Create viewport sampler") &&
           createImages(context, uiPass, extent);
}

bool ViewportTarget::resize(const VulkanContext& context, UiPass& uiPass, VkExtent2D extent) {
    if (extent.width == m_extent.width && extent.height == m_extent.height) {
        return true;
    }
    destroyImages(context, uiPass);
    return createImages(context, uiPass, extent);
}

bool ViewportTarget::createImages(const VulkanContext& context, UiPass& uiPass, VkExtent2D extent) {
    m_extent = extent;
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    for (ColorTarget& color : m_colors) {
        VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = m_colorFormat,
                                .extent = {extent.width, extent.height, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (!checkVulkan(
                vmaCreateImage(context.vmaAllocator(), &image, &allocation, &color.image, &color.allocation, nullptr),
                "Create viewport color image") ||
            !createView(context.device(), color.image, m_colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, color.view)) {
            return false;
        }
        color.texture = uiPass.addTexture(m_sampler, color.view);
        if (color.texture == 0) {
            return false;
        }
    }

    VkImageCreateInfo depth{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = m_depthFormat,
                            .extent = {extent.width, extent.height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    return checkVulkan(
               vmaCreateImage(context.vmaAllocator(), &depth, &allocation, &m_depthImage, &m_depthAllocation, nullptr),
               "Create viewport depth image") &&
           createView(context.device(), m_depthImage, m_depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthView);
}

void ViewportTarget::destroyImages(const VulkanContext& context, UiPass& uiPass) {
    for (ColorTarget& color : m_colors) {
        uiPass.removeTexture(color.texture);
        if (color.view) {
            vkDestroyImageView(context.device(), color.view, nullptr);
        }
        if (color.image) {
            vmaDestroyImage(context.vmaAllocator(), color.image, color.allocation);
        }
        color = {};
    }
    if (m_depthView) {
        vkDestroyImageView(context.device(), m_depthView, nullptr);
    }
    if (m_depthImage) {
        vmaDestroyImage(context.vmaAllocator(), m_depthImage, m_depthAllocation);
    }
    m_depthView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthAllocation = VK_NULL_HANDLE;
}

void ViewportTarget::transitionForSampling(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    ColorTarget& color = m_colors[frameIndex];
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = color.image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = 1,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    color.initialized = true;
}

void ViewportTarget::markWritten(uint32_t frameIndex) {
    m_colors[frameIndex].initialized = true;
}

void ViewportTarget::shutdown(const VulkanContext& context, UiPass& uiPass) {
    destroyImages(context, uiPass);
    if (m_sampler) {
        vkDestroySampler(context.device(), m_sampler, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_colors.clear();
}

VkImage ViewportTarget::image(uint32_t frameIndex) const {
    return m_colors[frameIndex].image;
}

VkImageView ViewportTarget::view(uint32_t frameIndex) const {
    return m_colors[frameIndex].view;
}

VkImageLayout ViewportTarget::oldLayout(uint32_t frameIndex) const {
    return m_colors[frameIndex].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
}

uint64_t ViewportTarget::texture(uint32_t frameIndex) const {
    return m_colors[frameIndex].texture;
}

VkImage ViewportTarget::depthImage() const {
    return m_depthImage;
}

VkImageView ViewportTarget::depthView() const {
    return m_depthView;
}

VkExtent2D ViewportTarget::extent() const {
    return m_extent;
}
