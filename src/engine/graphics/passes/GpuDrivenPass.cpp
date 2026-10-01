#include "GpuDrivenPass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {
uint32_t mipCount(VkExtent2D extent) {
    return 1u + static_cast<uint32_t>(std::floor(std::log2(std::max(extent.width, extent.height))));
}

struct DepthSettings {
    glm::ivec2 outputSize{0};
    int32_t sourceMip{0};
    int32_t copyDepth{0};
};

struct VisibilitySettings {
    glm::mat4 viewProjection{1.0f};
    glm::uvec4 counts{0u};
};
} // namespace

bool GpuDrivenPass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D extent, VkImageView depthView,
                         uint32_t maximumObjects, uint32_t maximumBatches) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = extent;
    m_maximumObjects = maximumObjects;
    m_maximumBatches = maximumBatches;
    if (!createLayouts() || !createPool(frameCount) || !createFrameBuffers(frameCount) ||
        !createDepthResources(extent, depthView)) {
        shutdown(context);
        return false;
    }
    ShaderModule depth(m_device);
    ShaderModule visibility(m_device);
    const std::filesystem::path shaders = shaderRoot();
    if (!depth.load(shaders / "depth_pyramid.comp.spv") || !visibility.load(shaders / "gpu_visibility.comp.spv")) {
        shutdown(context);
        return false;
    }
    if (!m_depthPipeline.init(m_device, m_depthLayout, depth.handle(), sizeof(DepthSettings)) ||
        !m_visibilityPipeline.init(m_device, m_visibilityLayout, visibility.handle(), sizeof(VisibilitySettings))) {
        shutdown(context);
        return false;
    }
    return true;
}

bool GpuDrivenPass::createLayouts() {
    const std::array<VkDescriptorSetLayoutBinding, 3> depthBindings{
        {{.binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 2,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}}};
    VkDescriptorSetLayoutCreateInfo depthInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = static_cast<uint32_t>(depthBindings.size()),
                                              .pBindings = depthBindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &depthInfo, nullptr, &m_depthLayout),
                     "Create depth-pyramid descriptor layout")) {
        return false;
    }
    const std::array<VkDescriptorSetLayoutBinding, 4> visibilityBindings{
        {{.binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 2,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 3,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}}};
    VkDescriptorSetLayoutCreateInfo visibilityInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = static_cast<uint32_t>(visibilityBindings.size()),
                                                   .pBindings = visibilityBindings.data()};
    return checkVulkan(vkCreateDescriptorSetLayout(m_device, &visibilityInfo, nullptr, &m_visibilityLayout),
                       "Create GPU-visibility descriptor layout");
}

bool GpuDrivenPass::createPool(uint32_t frameCount) {
    constexpr uint32_t levels = 32;
    const std::array<VkDescriptorPoolSize, 3> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, levels * 2 + frameCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, levels},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 3},
    }};
    VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                    .maxSets = levels + frameCount + 32,
                                    .poolSizeCount = static_cast<uint32_t>(sizes.size()),
                                    .pPoolSizes = sizes.data()};
    return checkVulkan(vkCreateDescriptorPool(m_device, &info, nullptr, &m_descriptorPool),
                       "Create GPU-driven descriptor pool");
}

bool GpuDrivenPass::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output) {
    VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, &info),
                     "Create GPU-driven buffer")) {
        return false;
    }
    output.mapped = info.pMappedData;
    return true;
}

bool GpuDrivenPass::createFrameBuffers(uint32_t frameCount) {
    m_frames.resize(frameCount);
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_visibilityLayout);
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = m_descriptorPool,
                                         .descriptorSetCount = frameCount,
                                         .pSetLayouts = layouts.data()};
    std::vector<VkDescriptorSet> descriptors(frameCount);
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, descriptors.data()),
                     "Allocate GPU-visibility descriptor sets")) {
        return false;
    }
    for (uint32_t index = 0; index < frameCount; ++index) {
        FrameData& frame = m_frames[index];
        frame.descriptor = descriptors[index];
        if (!createBuffer(sizeof(GpuDrivenObjectRecord) * m_maximumObjects, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          frame.objects) ||
            !createBuffer(sizeof(VkDrawIndexedIndirectCommand) * m_maximumBatches,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, frame.commands)) {
            return false;
        }
    }
    return true;
}

bool GpuDrivenPass::createDepthResources(VkExtent2D extent, VkImageView depthView) {
    m_extent = extent;
    const uint32_t levels = mipCount(extent);
    VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R32_SFLOAT,
                            .extent = {extent.width, extent.height, 1},
                            .mipLevels = levels,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_pyramid, &m_pyramidAllocation, nullptr),
                     "Create depth pyramid")) {
        return false;
    }
    VkImageViewCreateInfo fullView{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = m_pyramid,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = VK_FORMAT_R32_SFLOAT,
                                   .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .baseMipLevel = 0,
                                                        .levelCount = levels,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &fullView, nullptr, &m_pyramidView), "Create depth-pyramid view")) {
        return false;
    }
    m_mipViews.resize(levels);
    for (uint32_t level = 0; level < levels; ++level) {
        VkImageViewCreateInfo mipView = fullView;
        mipView.subresourceRange.baseMipLevel = level;
        mipView.subresourceRange.levelCount = 1;
        if (!checkVulkan(vkCreateImageView(m_device, &mipView, nullptr, &m_mipViews[level]),
                         "Create depth-pyramid mip view")) {
            return false;
        }
    }
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = static_cast<float>(levels)};
    if (!m_sampler &&
        !checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create depth-pyramid sampler")) {
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(levels, m_depthLayout);
    VkDescriptorSetAllocateInfo descriptorAllocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = m_descriptorPool,
                                                   .descriptorSetCount = levels,
                                                   .pSetLayouts = layouts.data()};
    m_depthDescriptors.resize(levels);
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &descriptorAllocate, m_depthDescriptors.data()),
                     "Allocate depth-pyramid descriptors")) {
        return false;
    }
    for (uint32_t level = 0; level < levels; ++level) {
        const VkDescriptorImageInfo sourceDepth{
            .sampler = m_sampler, .imageView = depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo sourcePyramid{
            .sampler = m_sampler, .imageView = m_pyramidView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo output{.imageView = m_mipViews[level], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 3> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_depthDescriptors[level],
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &sourceDepth},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_depthDescriptors[level],
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &sourcePyramid},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = m_depthDescriptors[level],
             .dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &output},
        }};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    m_pyramidInitialized = false;
    return true;
}

bool GpuDrivenPass::resize(const VulkanContext&, VkExtent2D extent, VkImageView depthView) {
    destroyDepthResources();
    return createDepthResources(extent, depthView);
}

bool GpuDrivenPass::prepare(uint32_t frameIndex, std::span<const GpuDrivenObjectRecord> objects,
                            std::span<const GpuDrivenBatch> batches, uint32_t referenceVisibleCount) {
    if (frameIndex >= m_frames.size() || objects.size() > m_maximumObjects || batches.size() > m_maximumBatches) {
        return false;
    }
    FrameData& frame = m_frames[frameIndex];
    if (frame.hasRetiredSubmission && frame.batchCount > 0) {
        vmaInvalidateAllocation(m_allocator, frame.commands.allocation, 0,
                                sizeof(VkDrawIndexedIndirectCommand) * frame.batchCount);
        const auto* retiredCommands = static_cast<const VkDrawIndexedIndirectCommand*>(frame.commands.mapped);
        uint32_t retiredVisibleCount = 0;
        for (uint32_t index = 0; index < frame.batchCount; ++index) {
            retiredVisibleCount += retiredCommands[index].instanceCount;
        }
        if (retiredVisibleCount > frame.referenceVisibleCount) {
            ++m_visibilityMismatchCount;
        }
    }
    if (!objects.empty()) {
        std::memcpy(frame.objects.mapped, objects.data(), objects.size_bytes());
    }
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(frame.commands.mapped);
    for (size_t index = 0; index < batches.size(); ++index) {
        commands[index] = {.indexCount = batches[index].indexCount,
                           .instanceCount = 0,
                           .firstIndex = batches[index].firstIndex,
                           .vertexOffset = batches[index].vertexOffset,
                           .firstInstance = batches[index].firstInstance};
    }
    frame.objectCount = static_cast<uint32_t>(objects.size());
    frame.batchCount = static_cast<uint32_t>(batches.size());
    frame.referenceVisibleCount = referenceVisibleCount;
    frame.hasRetiredSubmission = true;
    vmaFlushAllocation(m_allocator, frame.objects.allocation, 0, objects.size_bytes());
    vmaFlushAllocation(m_allocator, frame.commands.allocation, 0,
                       sizeof(VkDrawIndexedIndirectCommand) * batches.size());
    return true;
}

bool GpuDrivenPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkBuffer visibleObjectBuffer,
                           VkDeviceSize visibleObjectOffset, const glm::mat4& viewProjection, bool depthHistoryValid,
                           bool bypassOcclusion, VkQueryPool timestampQueries) {
    if (frameIndex >= m_frames.size()) {
        return false;
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 8);
    }
    VkImageMemoryBarrier2 pyramidBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            m_pyramidInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = m_pyramidInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = m_pyramidInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = m_pyramid,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = mipLevels(),
                             .baseArrayLayer = 0,
                             .layerCount = 1}};
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1,
                                .pImageMemoryBarriers = &pyramidBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    if (depthHistoryValid) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthPipeline.handle());
        VkExtent2D levelExtent = m_extent;
        for (uint32_t level = 0; level < mipLevels(); ++level) {
            const DepthSettings settings{
                .outputSize = {static_cast<int32_t>(levelExtent.width), static_cast<int32_t>(levelExtent.height)},
                .sourceMip = static_cast<int32_t>(level == 0 ? 0 : level - 1),
                .copyDepth = level == 0 ? 1 : 0};
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthPipeline.layout(), 0, 1,
                                    &m_depthDescriptors[level], 0, nullptr);
            vkCmdPushConstants(commandBuffer, m_depthPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(settings), &settings);
            vkCmdDispatch(commandBuffer, (levelExtent.width + 7) / 8, (levelExtent.height + 7) / 8, 1);
            VkImageMemoryBarrier2 levelBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                               .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                               .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .image = m_pyramid,
                                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                    .baseMipLevel = level,
                                                                    .levelCount = 1,
                                                                    .baseArrayLayer = 0,
                                                                    .layerCount = 1}};
            VkDependencyInfo levelDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                             .imageMemoryBarrierCount = 1,
                                             .pImageMemoryBarriers = &levelBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &levelDependency);
            levelExtent.width = std::max(levelExtent.width / 2, 1u);
            levelExtent.height = std::max(levelExtent.height / 2, 1u);
        }
    }
    m_pyramidInitialized = true;

    FrameData& frame = m_frames[frameIndex];
    const VkDescriptorBufferInfo records{
        .buffer = frame.objects.buffer, .offset = 0, .range = sizeof(GpuDrivenObjectRecord) * m_maximumObjects};
    const VkDescriptorBufferInfo visible{
        .buffer = visibleObjectBuffer, .offset = visibleObjectOffset, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo commands{
        .buffer = frame.commands.buffer, .offset = 0, .range = sizeof(VkDrawIndexedIndirectCommand) * m_maximumBatches};
    const VkDescriptorImageInfo pyramid{
        .sampler = m_sampler, .imageView = m_pyramidView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 4> writes{{
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.descriptor,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &records},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.descriptor,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &visible},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.descriptor,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &commands},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = frame.descriptor,
         .dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &pyramid},
    }};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    VkMemoryBarrier2 hostBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                 .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 .dstAccessMask =
                                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo hostDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &hostBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &hostDependency);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_visibilityPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_visibilityPipeline.layout(), 0, 1,
                            &frame.descriptor, 0, nullptr);
    const VisibilitySettings settings{
        .viewProjection = viewProjection,
        .counts = {frame.objectCount, mipLevels(), depthHistoryValid && !bypassOcclusion ? 1u : 0u, 0u}};
    vkCmdPushConstants(commandBuffer, m_visibilityPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    if (frame.objectCount > 0) {
        vkCmdDispatch(commandBuffer, (frame.objectCount + 63) / 64, 1, 1);
    }
    VkMemoryBarrier2 outputBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    VkDependencyInfo outputDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &outputBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &outputDependency);
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 9);
    }
    return true;
}

VkBuffer GpuDrivenPass::indirectBuffer(uint32_t frameIndex) const {
    return frameIndex < m_frames.size() ? m_frames[frameIndex].commands.buffer : VK_NULL_HANDLE;
}

uint32_t GpuDrivenPass::batchCount(uint32_t frameIndex) const {
    return frameIndex < m_frames.size() ? m_frames[frameIndex].batchCount : 0;
}

uint32_t GpuDrivenPass::mipLevels() const {
    return static_cast<uint32_t>(m_mipViews.size());
}

void GpuDrivenPass::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void GpuDrivenPass::destroyDepthResources() {
    if (!m_depthDescriptors.empty() && m_descriptorPool) {
        vkFreeDescriptorSets(m_device, m_descriptorPool, static_cast<uint32_t>(m_depthDescriptors.size()),
                             m_depthDescriptors.data());
    }
    m_depthDescriptors.clear();
    for (VkImageView view : m_mipViews) {
        if (view) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_mipViews.clear();
    if (m_pyramidView) {
        vkDestroyImageView(m_device, m_pyramidView, nullptr);
    }
    if (m_pyramid) {
        vmaDestroyImage(m_allocator, m_pyramid, m_pyramidAllocation);
    }
    m_pyramid = VK_NULL_HANDLE;
    m_pyramidView = VK_NULL_HANDLE;
    m_pyramidAllocation = VK_NULL_HANDLE;
    m_pyramidInitialized = false;
}

void GpuDrivenPass::shutdown(const VulkanContext&) {
    m_visibilityPipeline.shutdown(m_device);
    m_depthPipeline.shutdown(m_device);
    destroyDepthResources();
    for (FrameData& frame : m_frames) {
        destroyBuffer(frame.objects);
        destroyBuffer(frame.commands);
    }
    m_frames.clear();
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_descriptorPool) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    if (m_visibilityLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_visibilityLayout, nullptr);
    }
    if (m_depthLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_depthLayout, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_visibilityLayout = VK_NULL_HANDLE;
    m_depthLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_visibilityMismatchCount = 0;
}
