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
// Size of RenderObject in the mesh shaders: model, previous model, skin.
constexpr VkDeviceSize renderObjectSize = sizeof(glm::mat4) * 2 + sizeof(glm::uvec4);
constexpr VkDeviceSize drawCommandSize = sizeof(VkDrawIndexedIndirectCommand);

uint32_t groupsFor(uint32_t count) {
    return (count + workgroupSize - 1) / workgroupSize;
}

struct CullSettings {
    glm::mat4 viewProjection{1.0f};
    glm::uvec4 counts{0u};
    glm::uvec4 extents{0u};
    // xyz: camera position, w: LOD error scale (zero selects full detail).
    glm::vec4 lodView{0.0f};
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

bool GpuDrivenPass::init(const VulkanContext& context, uint32_t frameCount, const DepthPyramid& depthPyramid,
                         const GpuDrivenLimits& limits, bool indirectCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_limits = limits;
    m_indirectCount = indirectCount;
    if (!createLayouts() || !createPool(frameCount) || !createBuffers(frameCount)) {
        shutdown(context);
        return false;
    }
    bindDepthPyramid(depthPyramid);
    ShaderModule cull(m_device);
    ShaderModule compact(m_device);
    const std::filesystem::path shaders = shaderRoot();
    if (!cull.load(shaders / "gpu_visibility.comp.spv") || !compact.load(shaders / "compact_draws.comp.spv")) {
        shutdown(context);
        return false;
    }
    if (!m_cullPipeline.init(m_device, m_cullLayout, cull.handle(), context.pipelineCache(), sizeof(CullSettings)) ||
        !m_compactPipeline.init(m_device, m_compactLayout, compact.handle(), context.pipelineCache(),
                                sizeof(CompactSettings))) {
        shutdown(context);
        return false;
    }
    return true;
}

bool GpuDrivenPass::createLayouts() {
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
    return create(cullBindings, m_cullLayout, "Create GPU-cull descriptor layout") &&
           create(compactBindings, m_compactLayout, "Create draw-compaction descriptor layout");
}

bool GpuDrivenPass::createPool(uint32_t frameCount) {
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 10},
    }};
    VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = frameCount * 2,
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
    if (!createDeviceBuffer(renderObjectSize * m_limits.maximumInstances * phaseCount, storage, m_visibleObjects) ||
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

void GpuDrivenPass::bindDepthPyramid(const DepthPyramid& depthPyramid) {
    for (FrameData& frame : m_frames) {
        const VkDescriptorBufferInfo records{.buffer = frame.objects.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo visibleObjects{.buffer = m_visibleObjects.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo instanceCounts{.buffer = m_instanceCounts.buffer, .range = VK_WHOLE_SIZE};
        const VkDescriptorImageInfo pyramid{.sampler = depthPyramid.sampler(),
                                            .imageView = depthPyramid.view(),
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
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
}

void GpuDrivenPass::cull(VkCommandBuffer commandBuffer, uint32_t frameIndex, GpuCullPhase phase,
                         const glm::mat4& viewProjection, const DepthPyramid& depthPyramid,
                         const MeshLodView& lodView) {
    const FrameData& frame = m_frames[frameIndex];
    const uint32_t phaseIndex = static_cast<uint32_t>(phase);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline.layout(), 0, 1,
                            &frame.cullDescriptor, 0, nullptr);
    // The early phase never samples the pyramid, so a stale build is harmless.
    const VkExtent2D pyramidExtent = depthPyramid.renderExtent();
    const CullSettings cullSettings{
        .viewProjection = viewProjection,
        .counts = {frame.objectCount, depthPyramid.levelCount(), phaseIndex, m_limits.maximumInstances},
        .extents = {m_limits.maximumBatches, pyramidExtent.width, pyramidExtent.height, m_limits.maximumObjects},
        .lodView = {lodView.cameraPosition, lodView.errorScale}};
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
        .options = {m_limits.maximumInstances, m_indirectCount ? 1u : 0u, 0u, 0u}};
    vkCmdPushConstants(commandBuffer, m_compactPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(compactSettings), &compactSettings);
    if (frame.batchCount > 0) {
        vkCmdDispatch(commandBuffer, groupsFor(frame.batchCount), 1, 1);
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
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

void GpuDrivenPass::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void GpuDrivenPass::shutdown(const VulkanContext&) {
    m_cullPipeline.shutdown(m_device);
    m_compactPipeline.shutdown(m_device);
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
    if (m_descriptorPool) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    for (VkDescriptorSetLayout* layout : {&m_cullLayout, &m_compactLayout}) {
        if (*layout) {
            vkDestroyDescriptorSetLayout(m_device, *layout, nullptr);
        }
        *layout = VK_NULL_HANDLE;
    }
    m_descriptorPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_visibilityMismatchCount = 0;
    m_visibilityInitialized = false;
}
