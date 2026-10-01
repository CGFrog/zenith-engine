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
constexpr uint32_t phaseCount = 2;
constexpr uint32_t workgroupSize = 64;
constexpr uint32_t maximumPyramidLevels = 32;
// Size of RenderObject in the mesh shaders: model, previous model, skin.
constexpr VkDeviceSize renderObjectSize = sizeof(glm::mat4) * 2 + sizeof(glm::uvec4);
constexpr VkDeviceSize drawCommandSize = sizeof(VkDrawIndexedIndirectCommand);

uint32_t mipCount(VkExtent2D extent) {
    return 1u + static_cast<uint32_t>(std::floor(std::log2(std::max(extent.width, extent.height))));
}

uint32_t groupsFor(uint32_t count) {
    return (count + workgroupSize - 1) / workgroupSize;
}

struct DepthSettings {
    glm::ivec2 outputSize{0};
    int32_t sourceMip{0};
    int32_t copyDepth{0};
};

struct CullSettings {
    glm::mat4 viewProjection{1.0f};
    glm::uvec4 counts{0u};
    glm::uvec4 extents{0u};
};

struct CompactSettings {
    glm::uvec4 counts{0u};
    glm::uvec4 options{0u};
};

void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStages, VkAccessFlags2 sourceAccess,
                   VkPipelineStageFlags2 destinationStages, VkAccessFlags2 destinationAccess) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = sourceStages,
                                   .srcAccessMask = sourceAccess,
                                   .dstStageMask = destinationStages,
                                   .dstAccessMask = destinationAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type) {
    return {
        .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}
} // namespace

bool GpuDrivenPass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D extent, VkImageView depthView,
                         const GpuDrivenLimits& limits, bool indirectCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = extent;
    m_limits = limits;
    m_indirectCount = indirectCount;
    if (!createLayouts() || !createPool(frameCount) || !createBuffers(frameCount) ||
        !createDepthResources(extent, depthView)) {
        shutdown(context);
        return false;
    }
    ShaderModule depth(m_device);
    ShaderModule cull(m_device);
    ShaderModule compact(m_device);
    const std::filesystem::path shaders = shaderRoot();
    if (!depth.load(shaders / "depth_pyramid.comp.spv") || !cull.load(shaders / "gpu_visibility.comp.spv") ||
        !compact.load(shaders / "compact_draws.comp.spv")) {
        shutdown(context);
        return false;
    }
    if (!m_depthPipeline.init(m_device, m_depthLayout, depth.handle(), context.pipelineCache(),
                              sizeof(DepthSettings)) ||
        !m_cullPipeline.init(m_device, m_cullLayout, cull.handle(), context.pipelineCache(), sizeof(CullSettings)) ||
        !m_compactPipeline.init(m_device, m_compactLayout, compact.handle(), context.pipelineCache(),
                                sizeof(CompactSettings))) {
        shutdown(context);
        return false;
    }
    return true;
}

bool GpuDrivenPass::createLayouts() {
    const std::array<VkDescriptorSetLayoutBinding, 3> depthBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}};
    const std::array<VkDescriptorSetLayoutBinding, 7> cullBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)}};
    const std::array<VkDescriptorSetLayoutBinding, 4> compactBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)}};
    const auto create = [&](std::span<const VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout& layout,
                            const char* operation) {
        const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                   .pBindings = bindings.data()};
        return checkVulkan(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout), operation);
    };
    return create(depthBindings, m_depthLayout, "Create depth-pyramid descriptor layout") &&
           create(cullBindings, m_cullLayout, "Create GPU-cull descriptor layout") &&
           create(compactBindings, m_compactLayout, "Create draw-compaction descriptor layout");
}

bool GpuDrivenPass::createPool(uint32_t frameCount) {
    const std::array<VkDescriptorPoolSize, 3> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maximumPyramidLevels * 2 + frameCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maximumPyramidLevels},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 10},
    }};
    VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                    .maxSets = maximumPyramidLevels + frameCount * 2,
                                    .poolSizeCount = static_cast<uint32_t>(sizes.size()),
                                    .pPoolSizes = sizes.data()};
    return checkVulkan(vkCreateDescriptorPool(m_device, &info, nullptr, &m_descriptorPool),
                       "Create GPU-driven descriptor pool");
}

bool GpuDrivenPass::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output) {
    VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    // Random access also suits the statistics buffer the host reads back.
    VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, &info),
                     "Create GPU-driven host buffer")) {
        return false;
    }
    output.mapped = info.pMappedData;
    return true;
}

bool GpuDrivenPass::createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& output) {
    VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, nullptr),
                       "Create GPU-driven device buffer");
}

bool GpuDrivenPass::createBuffers(uint32_t frameCount) {
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags cleared = storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!createDeviceBuffer(renderObjectSize * m_limits.maximumObjects * phaseCount, storage, m_visibleObjects) ||
        !createDeviceBuffer(sizeof(uint32_t) * m_limits.maximumBatches * phaseCount, cleared, m_instanceCounts) ||
        !createDeviceBuffer(drawCommandSize * m_limits.maximumBatches * phaseCount,
                            storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, m_drawCommands) ||
        !createDeviceBuffer(sizeof(uint32_t) * m_limits.maximumGroups * phaseCount,
                            cleared | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, m_drawCounts) ||
        !createDeviceBuffer(sizeof(uint32_t) * m_limits.maximumObjects, cleared, m_visibility) ||
        !createDeviceBuffer(sizeof(uint32_t) * 4, cleared | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, m_statistics)) {
        return false;
    }
    m_frames.resize(frameCount);
    std::vector<VkDescriptorSetLayout> layouts;
    for (uint32_t index = 0; index < frameCount; ++index) {
        layouts.push_back(m_cullLayout);
        layouts.push_back(m_compactLayout);
    }
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = m_descriptorPool,
                                         .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
                                         .pSetLayouts = layouts.data()};
    std::vector<VkDescriptorSet> descriptors(layouts.size());
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, descriptors.data()),
                     "Allocate GPU-driven descriptor sets")) {
        return false;
    }
    for (uint32_t index = 0; index < frameCount; ++index) {
        FrameData& frame = m_frames[index];
        frame.cullDescriptor = descriptors[index * 2];
        frame.compactDescriptor = descriptors[index * 2 + 1];
        if (!createHostBuffer(sizeof(GpuDrivenObjectRecord) * m_limits.maximumObjects, storage, frame.objects) ||
            !createHostBuffer(sizeof(GpuDrivenBatch) * m_limits.maximumBatches, storage, frame.batches) ||
            !createHostBuffer(sizeof(uint32_t) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, frame.statistics)) {
            return false;
        }
    }
    return true;
}

bool GpuDrivenPass::createDepthResources(VkExtent2D extent, VkImageView depthView) {
    m_extent = extent;
    const uint32_t levels = std::min(mipCount(extent), maximumPyramidLevels);
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
                                .maxLod = static_cast<float>(maximumPyramidLevels)};
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
    writeCullDescriptors();
    return true;
}

void GpuDrivenPass::writeCullDescriptors() {
    for (FrameData& frame : m_frames) {
        const VkDescriptorBufferInfo records{.buffer = frame.objects.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo visibleObjects{.buffer = m_visibleObjects.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo instanceCounts{.buffer = m_instanceCounts.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorImageInfo pyramid{
            .sampler = m_sampler, .imageView = m_pyramidView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo visibility{.buffer = m_visibility.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo statistics{.buffer = m_statistics.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo batches{.buffer = frame.batches.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo drawCommands{.buffer = m_drawCommands.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo drawCounts{.buffer = m_drawCounts.buffer, .range = VK_WHOLE_SIZE};
        const auto bufferWrite = [](VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* info) {
            return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = set,
                                        .dstBinding = binding,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .pBufferInfo = info};
        };
        const std::array<VkWriteDescriptorSet, 11> writes{{
            bufferWrite(frame.cullDescriptor, 0, &records),
            bufferWrite(frame.cullDescriptor, 1, &visibleObjects),
            bufferWrite(frame.cullDescriptor, 2, &instanceCounts),
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = frame.cullDescriptor,
             .dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &pyramid},
            bufferWrite(frame.cullDescriptor, 4, &visibility),
            bufferWrite(frame.cullDescriptor, 5, &statistics),
            bufferWrite(frame.cullDescriptor, 6, &batches),
            bufferWrite(frame.compactDescriptor, 0, &batches),
            bufferWrite(frame.compactDescriptor, 1, &instanceCounts),
            bufferWrite(frame.compactDescriptor, 2, &drawCommands),
            bufferWrite(frame.compactDescriptor, 3, &drawCounts),
        }};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

bool GpuDrivenPass::resize(const VulkanContext&, VkExtent2D extent, VkImageView depthView) {
    destroyDepthResources();
    return createDepthResources(extent, depthView);
}

bool GpuDrivenPass::prepare(uint32_t frameIndex, std::span<const GpuDrivenObjectRecord> objects,
                            std::span<const GpuDrivenBatch> batches, uint32_t referenceVisibleCount) {
    if (frameIndex >= m_frames.size() || objects.size() > m_limits.maximumObjects ||
        batches.size() > m_limits.maximumBatches) {
        return false;
    }
    FrameData& frame = m_frames[frameIndex];
    // The caller waited for this frame slot, so its last statistics are final.
    if (frame.statisticsPending) {
        vmaInvalidateAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(uint32_t) * 2);
        const auto* emitted = static_cast<const uint32_t*>(frame.statistics.mapped);
        m_retiredVisibleCount = emitted[0] + emitted[1];
        // The GPU never sees more than the CPU frustum reference for the frame.
        if (m_retiredVisibleCount > frame.referenceVisibleCount) {
            ++m_visibilityMismatchCount;
        }
        frame.statisticsPending = false;
    }
    if (!objects.empty()) {
        std::memcpy(frame.objects.mapped, objects.data(), objects.size_bytes());
        vmaFlushAllocation(m_allocator, frame.objects.allocation, 0, objects.size_bytes());
    }
    if (!batches.empty()) {
        std::memcpy(frame.batches.mapped, batches.data(), batches.size_bytes());
        vmaFlushAllocation(m_allocator, frame.batches.allocation, 0, batches.size_bytes());
    }
    frame.objectCount = static_cast<uint32_t>(objects.size());
    frame.batchCount = static_cast<uint32_t>(batches.size());
    frame.referenceVisibleCount = referenceVisibleCount;
    return true;
}

void GpuDrivenPass::beginFrame(VkCommandBuffer commandBuffer, uint32_t) {
    // The previous frame may still read draw commands and visible instances, or
    // write counters; resetting them must wait for that work.
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(commandBuffer, m_instanceCounts.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(commandBuffer, m_drawCounts.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(commandBuffer, m_statistics.buffer, 0, VK_WHOLE_SIZE, 0);
    if (!m_visibilityInitialized) {
        // Nothing was visible "last frame", so the first late phase draws all.
        vkCmdFillBuffer(commandBuffer, m_visibility.buffer, 0, VK_WHOLE_SIZE, 0);
        m_visibilityInitialized = true;
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (!m_pyramidInitialized) {
        // The cull shader statically references the pyramid in both phases.
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .image = m_pyramid,
                                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                 .baseMipLevel = 0,
                                                                 .levelCount = mipLevels(),
                                                                 .baseArrayLayer = 0,
                                                                 .layerCount = 1}};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        m_pyramidInitialized = true;
    }
}

void GpuDrivenPass::cull(VkCommandBuffer commandBuffer, uint32_t frameIndex, GpuCullPhase phase,
                         const glm::mat4& viewProjection, VkExtent2D renderExtent) {
    const FrameData& frame = m_frames[frameIndex];
    const uint32_t phaseIndex = static_cast<uint32_t>(phase);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline.layout(), 0, 1,
                            &frame.cullDescriptor, 0, nullptr);
    const CullSettings cullSettings{.viewProjection = viewProjection,
                                    .counts = {frame.objectCount, mipLevels(), phaseIndex, m_limits.maximumObjects},
                                    .extents = {m_limits.maximumBatches, std::min(renderExtent.width, m_extent.width),
                                                std::min(renderExtent.height, m_extent.height),
                                                m_limits.maximumObjects}};
    vkCmdPushConstants(commandBuffer, m_cullPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cullSettings),
                       &cullSettings);
    if (frame.objectCount > 0) {
        vkCmdDispatch(commandBuffer, groupsFor(frame.objectCount), 1, 1);
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_compactPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_compactPipeline.layout(), 0, 1,
                            &frame.compactDescriptor, 0, nullptr);
    const CompactSettings compactSettings{
        .counts = {frame.batchCount, phaseIndex, m_limits.maximumBatches, m_limits.maximumGroups},
        .options = {m_limits.maximumObjects, m_indirectCount ? 1u : 0u, 0u, 0u}};
    vkCmdPushConstants(commandBuffer, m_compactPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(compactSettings), &compactSettings);
    if (frame.batchCount > 0) {
        vkCmdDispatch(commandBuffer, groupsFor(frame.batchCount), 1, 1);
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void GpuDrivenPass::buildDepthPyramid(VkCommandBuffer commandBuffer) {
    // The early cull sampled the pyramid (statically); rebuild after it.
    const VkImageMemoryBarrier2 pyramidBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                               .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                               .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .image = m_pyramid,
                                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                    .baseMipLevel = 0,
                                                                    .levelCount = mipLevels(),
                                                                    .baseArrayLayer = 0,
                                                                    .layerCount = 1}};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &pyramidBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthPipeline.handle());
    VkExtent2D levelExtent = m_extent;
    for (uint32_t level = 0; level < mipLevels(); ++level) {
        const DepthSettings settings{
            .outputSize = {static_cast<int32_t>(levelExtent.width), static_cast<int32_t>(levelExtent.height)},
            .sourceMip = static_cast<int32_t>(level == 0 ? 0 : level - 1),
            .copyDepth = level == 0 ? 1 : 0};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthPipeline.layout(), 0, 1,
                                &m_depthDescriptors[level], 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_depthPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                           &settings);
        vkCmdDispatch(commandBuffer, (levelExtent.width + 7) / 8, (levelExtent.height + 7) / 8, 1);
        // Each level feeds the next level and, finally, the late cull.
        const VkImageMemoryBarrier2 levelBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
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
        const VkDependencyInfo levelDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                               .imageMemoryBarrierCount = 1,
                                               .pImageMemoryBarriers = &levelBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &levelDependency);
        levelExtent.width = std::max(levelExtent.width / 2, 1u);
        levelExtent.height = std::max(levelExtent.height / 2, 1u);
    }
}

void GpuDrivenPass::drawGroup(VkCommandBuffer commandBuffer, GpuCullPhase phase, uint32_t group, uint32_t firstBatch,
                              uint32_t batchCount) const {
    const uint32_t phaseIndex = static_cast<uint32_t>(phase);
    const VkDeviceSize commandOffset = drawCommandSize * (phaseIndex * m_limits.maximumBatches + firstBatch);
    if (m_indirectCount) {
        const VkDeviceSize countOffset = sizeof(uint32_t) * (phaseIndex * m_limits.maximumGroups + group);
        vkCmdDrawIndexedIndirectCount(commandBuffer, m_drawCommands.buffer, commandOffset, m_drawCounts.buffer,
                                      countOffset, batchCount, static_cast<uint32_t>(drawCommandSize));
    } else {
        vkCmdDrawIndexedIndirect(commandBuffer, m_drawCommands.buffer, commandOffset, batchCount,
                                 static_cast<uint32_t>(drawCommandSize));
    }
}

void GpuDrivenPass::finishFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    FrameData& frame = m_frames[frameIndex];
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy copy{.size = sizeof(uint32_t) * 2};
    vkCmdCopyBuffer(commandBuffer, m_statistics.buffer, frame.statistics.buffer, 1, &copy);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    frame.statisticsPending = true;
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
    m_cullPipeline.shutdown(m_device);
    m_compactPipeline.shutdown(m_device);
    m_depthPipeline.shutdown(m_device);
    destroyDepthResources();
    for (FrameData& frame : m_frames) {
        destroyBuffer(frame.objects);
        destroyBuffer(frame.batches);
        destroyBuffer(frame.statistics);
    }
    m_frames.clear();
    for (Buffer* buffer :
         {&m_visibleObjects, &m_instanceCounts, &m_drawCommands, &m_drawCounts, &m_visibility, &m_statistics}) {
        destroyBuffer(*buffer);
    }
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_descriptorPool) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    for (VkDescriptorSetLayout* layout : {&m_cullLayout, &m_compactLayout, &m_depthLayout}) {
        if (*layout) {
            vkDestroyDescriptorSetLayout(m_device, *layout, nullptr);
        }
        *layout = VK_NULL_HANDLE;
    }
    m_sampler = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_visibilityMismatchCount = 0;
    m_visibilityInitialized = false;
}
