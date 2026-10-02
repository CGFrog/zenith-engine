#include "TemporalResolvePass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>

namespace {
constexpr uint32_t workgroupSize = 8;
constexpr uint32_t sampledInputCount = 4;

// Mirrors ResolveSettings in shaders/taa_resolve.comp.
struct ResolveSettings {
    glm::mat4 currentToPreviousClip{1.0f};
    // The region this frame resolves.
    glm::vec4 outputSize{0.0f};
    glm::vec4 renderSize{0.0f};
    // xy: the previous frame's resolved region / history image extent,
    // zw: the previous render extent / scene image extent.
    glm::vec4 regionScale{0.0f};
    glm::vec2 jitterPixels{0.0f};
    float historyValid{0.0f};
    float previousDepthValid{0.0f};
};

glm::vec4 sizeAndReciprocal(VkExtent2D extent) {
    const float width = static_cast<float>(std::max(extent.width, 1u));
    const float height = static_cast<float>(std::max(extent.height, 1u));
    return {width, height, 1.0f / width, 1.0f / height};
}

glm::vec2 regionScale(VkExtent2D region, VkExtent2D full) {
    return {static_cast<float>(region.width) / static_cast<float>(std::max(full.width, 1u)),
            static_cast<float>(region.height) / static_cast<float>(std::max(full.height, 1u))};
}

VkImageMemoryBarrier2 historyBarrier(VkImage image, VkImageLayout oldLayout, VkPipelineStageFlags2 sourceStages,
                                     VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStages,
                                     VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStages,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStages,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = TemporalResolvePass::resolvedLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}};
}
} // namespace

bool TemporalResolvePass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = outputExtent;
    ShaderModule shader(m_device);
    return createDescriptors(frameCount) && shader.load(shaderRoot() / "taa_resolve.comp.spv") &&
           m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(ResolveSettings));
}

bool TemporalResolvePass::createDescriptors(uint32_t frameCount) {
    std::array<VkDescriptorSetLayoutBinding, sampledInputCount + 1> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {.binding = binding,
                             .descriptorType = binding < sampledInputCount ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             .descriptorCount = 1,
                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create temporal resolve descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize sizes[]{
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * sampledInputCount},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = frameCount}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = frameCount,
                                          .poolSizeCount = static_cast<uint32_t>(std::size(sizes)),
                                          .pPoolSizes = sizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool),
                     "Create temporal resolve descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    m_descriptors.resize(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_descriptors.data()),
                     "Allocate temporal resolve descriptors")) {
        return false;
    }
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create temporal resolve sampler");
}

bool TemporalResolvePass::ensureHistory(VkCommandBuffer commandBuffer) {
    if (m_history[0].image) {
        return true;
    }
    m_latest = 0;
    m_historyWritten = false;
    for (HistoryImage& history : m_history) {
        const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = historyFormat,
                                      .extent = {.width = m_extent.width, .height = m_extent.height, .depth = 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &history.image, &history.allocation, nullptr),
                         "Create temporal history image")) {
            destroyHistory();
            return false;
        }
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = history.image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = historyFormat,
                                         .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .baseMipLevel = 0,
                                                              .levelCount = 1,
                                                              .baseArrayLayer = 0,
                                                              .layerCount = 1}};
        if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &history.view), "Create temporal history view")) {
            destroyHistory();
            return false;
        }
    }
    const VkPipelineStageFlags2 readers =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkImageMemoryBarrier2 barriers[]{
        historyBarrier(m_history[0].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, readers, 0),
        historyBarrier(m_history[1].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, readers, 0)};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = static_cast<uint32_t>(std::size(barriers)),
                                      .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    return true;
}

void TemporalResolvePass::destroyHistory() {
    for (HistoryImage& history : m_history) {
        if (history.view) {
            vkDestroyImageView(m_device, history.view, nullptr);
        }
        if (history.image) {
            vmaDestroyImage(m_allocator, history.image, history.allocation);
        }
        history = {};
    }
    m_latest = 0;
    m_latestExtent = {};
    m_historyWritten = false;
}

void TemporalResolvePass::resize(VkExtent2D outputExtent) {
    destroyHistory();
    m_extent = outputExtent;
}

void TemporalResolvePass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroyHistory();
    m_pipeline.shutdown(m_device);
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_descriptors.clear();
}

void TemporalResolvePass::writeDescriptors(VkDescriptorSet descriptor, const TemporalResolveImages& images,
                                           uint32_t readIndex, uint32_t writeIndex) const {
    const VkDescriptorImageInfo inputs[sampledInputCount]{
        {.sampler = m_sampler, .imageView = images.sceneColor, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = m_sampler, .imageView = images.motion, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = m_sampler,
         .imageView = images.previousMotion,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = m_sampler, .imageView = m_history[readIndex].view, .imageLayout = resolvedLayout}};
    const VkDescriptorImageInfo output{.imageView = m_history[writeIndex].view, .imageLayout = resolvedLayout};
    std::array<VkWriteDescriptorSet, sampledInputCount + 1> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        const bool storage = binding == sampledInputCount;
        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                           .dstSet = descriptor,
                           .dstBinding = binding,
                           .descriptorCount = 1,
                           .descriptorType =
                               storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           .pImageInfo = storage ? &output : &inputs[binding]};
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool TemporalResolvePass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                 const TemporalResolveImages& images, const TemporalResolveFrame& frame) {
    if (!ensureHistory(commandBuffer)) {
        return false;
    }
    const uint32_t readIndex = m_latest;
    const uint32_t writeIndex = m_latest ^ 1u;
    const VkDescriptorSet descriptor = m_descriptors[frameIndex];
    writeDescriptors(descriptor, images, readIndex, writeIndex);

    // Scene color and motion were last made visible to fragment shaders; chain
    // that dependency on to this compute pass. The previous frame's motion and
    // history were written in earlier submissions on the same queue.
    const VkMemoryBarrier2 inputs{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    const VkImageMemoryBarrier2 history[]{
        historyBarrier(m_history[readIndex].image, resolvedLayout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        // Earlier frames read this image as history (compute) and as the
        // resolved scene (fragment); every texel is overwritten below.
        historyBarrier(m_history[writeIndex].image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)};
    const VkDependencyInfo before{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .memoryBarrierCount = 1,
                                  .pMemoryBarriers = &inputs,
                                  .imageMemoryBarrierCount = static_cast<uint32_t>(std::size(history)),
                                  .pImageMemoryBarriers = history};
    vkCmdPipelineBarrier2(commandBuffer, &before);

    const VkExtent2D resolveExtent = images.resolveExtent.width == 0 || images.resolveExtent.height == 0
                                         ? m_extent
                                         : VkExtent2D{std::clamp(images.resolveExtent.width, 1u, m_extent.width),
                                                      std::clamp(images.resolveExtent.height, 1u, m_extent.height)};
    const bool historyValid = frame.historyValid && m_historyWritten;
    const ResolveSettings settings{.currentToPreviousClip = frame.currentToPreviousClip,
                                   .outputSize = sizeAndReciprocal(resolveExtent),
                                   .renderSize = sizeAndReciprocal(images.renderExtent),
                                   .regionScale = {regionScale(historyValid ? m_latestExtent : resolveExtent, m_extent),
                                                   regionScale(images.previousRenderExtent, m_extent)},
                                   .jitterPixels = frame.jitterPixels,
                                   .historyValid = historyValid ? 1.0f : 0.0f,
                                   .previousDepthValid = images.previousMotionValid ? 1.0f : 0.0f};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1, &descriptor, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(commandBuffer, (resolveExtent.width + workgroupSize - 1) / workgroupSize,
                  (resolveExtent.height + workgroupSize - 1) / workgroupSize, 1);

    const VkImageMemoryBarrier2 resolved =
        historyBarrier(m_history[writeIndex].image, resolvedLayout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const VkDependencyInfo after{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &resolved};
    vkCmdPipelineBarrier2(commandBuffer, &after);
    m_latest = writeIndex;
    m_latestExtent = resolveExtent;
    m_historyWritten = true;
    return true;
}

uint64_t TemporalResolvePass::historyBytes() const {
    if (!m_history[0].image) {
        return 0;
    }
    return static_cast<uint64_t>(m_extent.width) * m_extent.height * sizeof(uint16_t) * 4 * m_history.size();
}
