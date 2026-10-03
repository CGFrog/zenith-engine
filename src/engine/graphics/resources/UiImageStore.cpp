#include "UiImageStore.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <cstring>
#include <utility>

namespace {
void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStage,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStage,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = newLayout,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = image,
                                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                             .baseMipLevel = 0,
                                                             .levelCount = 1,
                                                             .baseArrayLayer = 0,
                                                             .layerCount = 1}};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

bool createSampler(VkDevice device, VkFilter filter, VkSampler& sampler) {
    const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = filter,
                                   .minFilter = filter,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(device, &info, nullptr, &sampler), "Create UI image sampler");
}
} // namespace

bool UiImageStore::init(const VulkanContext& context, UiPass& uiPass) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_uiPass = &uiPass;
    // UI images are sRGB-encoded like ImGui colours; an sRGB target expects
    // linear values from the shader, so the image decodes on sampling.
    m_format = uiPass.targetIsSrgb() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return createSampler(m_device, VK_FILTER_LINEAR, m_linearSampler) &&
           createSampler(m_device, VK_FILTER_NEAREST, m_nearestSampler);
}

void UiImageStore::sync(VkCommandBuffer commandBuffer, uint64_t recordingSerial, RenderRuntime& rendering) {
    std::vector<RenderRuntime::UiImageSlot>& slots = rendering.m_uiImages;
    if (m_images.size() < slots.size()) {
        m_images.resize(slots.size());
    }
    for (size_t index = 0; index < slots.size(); ++index) {
        RenderRuntime::UiImageSlot& slot = slots[index];
        GpuImage& current = m_images[index];
        // UI draw data of this frame may still sample a destroyed image.
        if (current.image != VK_NULL_HANDLE && (!slot.alive || current.generation != slot.generation)) {
            retire(recordingSerial, std::exchange(current, {}));
        }
        if (!slot.alive || !slot.pending) {
            continue;
        }
        GpuImage replacement{.generation = slot.generation};
        const bool uploaded = upload(commandBuffer, recordingSerial, *slot.pending, replacement);
        // A failed upload keeps the previous texture rather than retrying every frame.
        slot.pending.reset();
        if (!uploaded) {
            // The upload may have recorded a copy into the image before
            // failing, so it can only be destroyed after this frame.
            retire(recordingSerial, replacement);
            continue;
        }
        if (current.image != VK_NULL_HANDLE) {
            retire(recordingSerial, current);
        }
        current = replacement;
        slot.texture = {current.texture};
    }
}

void UiImageStore::collect(uint64_t completedSerial) {
    m_releases.collect(completedSerial);
}

void UiImageStore::shutdown() {
    m_releases.collect(UINT64_MAX);
    for (GpuImage& image : m_images) {
        destroy(image);
    }
    m_images.clear();
    if (m_device != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_linearSampler, nullptr);
        vkDestroySampler(m_device, m_nearestSampler, nullptr);
    }
    m_linearSampler = VK_NULL_HANDLE;
    m_nearestSampler = VK_NULL_HANDLE;
}

bool UiImageStore::upload(VkCommandBuffer commandBuffer, uint64_t recordingSerial, const UiImage& pixels,
                          GpuImage& image) {
    const auto byteCount = static_cast<VkDeviceSize>(pixels.rgba.size());
    const VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = byteCount, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo stagingAllocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                                    .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMemory = VK_NULL_HANDLE;
    VmaAllocationInfo stagingDetails{};
    if (!checkVulkan(
            vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocation, &staging, &stagingMemory, &stagingDetails),
            "Create UI image staging buffer")) {
        return false;
    }
    // The copy executes with this frame, so the buffer retires with it.
    m_releases.defer(recordingSerial, [allocator = m_allocator, staging, stagingMemory] {
        vmaDestroyBuffer(allocator, staging, stagingMemory);
    });
    std::memcpy(stagingDetails.pMappedData, pixels.rgba.data(), byteCount);
    if (!checkVulkan(vmaFlushAllocation(m_allocator, stagingMemory, 0, byteCount), "Flush UI image staging buffer")) {
        return false;
    }

    const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = m_format,
                                      .extent = {pixels.width, pixels.height, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &imageInfo, &allocation, &image.image, &image.allocation, nullptr),
                     "Create UI image")) {
        return false;
    }
    const VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = image.image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = m_format,
                                         .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .baseMipLevel = 0,
                                                              .levelCount = 1,
                                                              .baseArrayLayer = 0,
                                                              .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Create UI image view")) {
        return false;
    }
    imageBarrier(commandBuffer, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                      .mipLevel = 0,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1},
                                 .imageExtent = {pixels.width, pixels.height, 1}};
    vkCmdCopyBufferToImage(commandBuffer, staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const VkSampler sampler = pixels.filter == UiImageFilter::Nearest ? m_nearestSampler : m_linearSampler;
    image.texture = m_uiPass->addTexture(sampler, image.view);
    return image.texture != 0;
}

void UiImageStore::retire(uint64_t lastUseSerial, GpuImage image) {
    m_releases.defer(lastUseSerial, [this, image]() mutable { destroy(image); });
}

void UiImageStore::destroy(GpuImage& image) {
    if (image.texture != 0) {
        m_uiPass->removeTexture(image.texture);
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
    image = {};
}
