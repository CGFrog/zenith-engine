#include "ClusteredLightingPass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr uint32_t workgroupSize = 64;
constexpr uint32_t bindingCount = 4;

// Mirrors ClusterSettings in shaders/light_clusters.comp.
struct ClusterSettings {
    glm::mat4 view{1.0f};
    glm::vec4 projection{0.0f};
    glm::vec4 depth{0.0f};
    glm::uvec4 grid{0u};
};

// Mirrors Statistics in shaders/light_clusters.comp.
struct StatisticsRecord {
    uint32_t maximumLightsInCluster{0};
    uint32_t discardedAssignments{0};
};

GpuLocalLight packLight(const LocalLight& light) {
    return {.positionRange = {light.position, light.range},
            .colorIntensity = {light.color, light.intensity},
            .directionType = {light.direction, light.type == LocalLightType::Spot ? 1.0f : 0.0f},
            .spotCone = {light.innerConeCosine, light.outerConeCosine, 0.0f, 0.0f}};
}
} // namespace

bool ClusteredLightingPass::init(const VulkanContext& context, uint32_t frameCount,
                                 const ClusterGridConfiguration& configuration, uint32_t lightCapacity) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_configuration = makeClusterGrid(configuration, glm::mat4(1.0f), {1, 1}, 1.0f, 2.0f).configuration;
    m_lightCapacity = std::max(lightCapacity, 1u);
    if (!createDescriptors(frameCount)) {
        shutdown();
        return false;
    }
    ShaderModule shader(m_device);
    if (!shader.load(shaderRoot() / "light_clusters.comp.spv") ||
        !m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(ClusterSettings))) {
        shutdown();
        return false;
    }
    return true;
}

bool ClusteredLightingPass::createDescriptors(uint32_t frameCount) {
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (uint32_t binding = 0; binding < bindingCount; ++binding) {
        bindings[binding] = {.binding = binding,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .descriptorCount = 1,
                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                     .bindingCount = bindingCount,
                                                     .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
                     "Create light-cluster descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .descriptorCount = frameCount * bindingCount};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = 1,
                                              .pPoolSizes = &poolSize};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool),
                     "Create light-cluster descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    std::vector<VkDescriptorSet> sets(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate light-cluster descriptor sets")) {
        return false;
    }
    m_frames.resize(frameCount);
    for (uint32_t index = 0; index < frameCount; ++index) {
        FrameData& frame = m_frames[index];
        frame.descriptor = sets[index];
        if (!createFrameBuffers(frame)) {
            return false;
        }
        const std::array<VkDescriptorBufferInfo, bindingCount> buffers{
            {{.buffer = frame.lights.buffer, .range = VK_WHOLE_SIZE},
             {.buffer = frame.clusterRanges.buffer, .range = VK_WHOLE_SIZE},
             {.buffer = frame.lightIndices.buffer, .range = VK_WHOLE_SIZE},
             {.buffer = frame.statistics.buffer, .range = VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, bindingCount> writes{};
        for (uint32_t binding = 0; binding < bindingCount; ++binding) {
            writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = frame.descriptor,
                               .dstBinding = binding,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &buffers[binding]};
        }
        vkUpdateDescriptorSets(m_device, bindingCount, writes.data(), 0, nullptr);
    }
    return true;
}

bool ClusteredLightingPass::createFrameBuffers(FrameData& frame) {
    const uint32_t clusterCount = m_configuration.columns * m_configuration.rows * m_configuration.depthSlices;
    if (!createBuffer(sizeof(GpuLocalLight) * m_lightCapacity, BufferAccess::HostWrite, frame.lights) ||
        !createBuffer(sizeof(glm::uvec2) * clusterCount, BufferAccess::Device, frame.clusterRanges) ||
        !createBuffer(sizeof(uint32_t) * clusterCount * maximumLightsPerCluster, BufferAccess::Device,
                      frame.lightIndices) ||
        !createBuffer(sizeof(StatisticsRecord), BufferAccess::HostReadback, frame.statistics)) {
        return false;
    }
    std::memset(frame.statistics.mapped, 0, sizeof(StatisticsRecord));
    vmaFlushAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
    return true;
}

bool ClusteredLightingPass::createBuffer(VkDeviceSize size, BufferAccess access, Buffer& output) {
    const VkBufferCreateInfo buffer{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    // Fragment shaders read the light list heavily, so it prefers device-local
    // host-visible memory; the small statistics record is read back by the host.
    if (access == BufferAccess::HostWrite) {
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else if (access == BufferAccess::HostReadback) {
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
    }
    VmaAllocationInfo info{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, &info),
                     "Create light-cluster buffer")) {
        return false;
    }
    output.mapped = info.pMappedData;
    return true;
}

void ClusteredLightingPass::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

uint32_t ClusteredLightingPass::beginFrame(uint32_t frameIndex, std::span<const LocalLight> lights,
                                           ClusterLightStatistics& retired) {
    FrameData& frame = m_frames[frameIndex];
    // The caller waited for this slot, so its last assignment is complete.
    retired = {};
    if (frame.statisticsPending) {
        vmaInvalidateAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
        StatisticsRecord record;
        std::memcpy(&record, frame.statistics.mapped, sizeof(record));
        retired = {.maximumLightsInCluster = record.maximumLightsInCluster,
                   .discardedAssignments = record.discardedAssignments};
        std::memset(frame.statistics.mapped, 0, sizeof(StatisticsRecord));
        vmaFlushAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
        frame.statisticsPending = false;
    }
    frame.lightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), m_lightCapacity));
    auto* records = static_cast<GpuLocalLight*>(frame.lights.mapped);
    for (uint32_t index = 0; index < frame.lightCount; ++index) {
        records[index] = packLight(lights[index]);
    }
    if (frame.lightCount > 0) {
        vmaFlushAllocation(m_allocator, frame.lights.allocation, 0, sizeof(GpuLocalLight) * frame.lightCount);
    }
    return frame.lightCount;
}

void ClusteredLightingPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const ClusterGrid& grid,
                                   const glm::mat4& view) {
    FrameData& frame = m_frames[frameIndex];
    if (frame.lightCount == 0) {
        return;
    }
    // The buffers are sized for the configured grid, which the caller's grid
    // must have been built from.
    const ClusterGridConfiguration& gridSize = m_configuration;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1, &frame.descriptor,
                            0, nullptr);
    const ClusterSettings settings{.view = view,
                                   .projection = {grid.projectionScale, grid.projectionOffset},
                                   .depth = {grid.nearDepth, grid.sliceEndDepth, grid.farDepth, 0.0f},
                                   .grid = {gridSize.columns, gridSize.rows, gridSize.depthSlices, frame.lightCount}};
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    const uint32_t clusterCount = gridSize.columns * gridSize.rows * gridSize.depthSlices;
    vkCmdDispatch(commandBuffer, (clusterCount + workgroupSize - 1) / workgroupSize, 1, 1);
    frame.statisticsPending = true;
    // Lists feed this frame's fragment shading and volumetric fog injection
    // (compute); statistics are read by the host once the frame's fence signals.
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_HOST_BIT,
                                   .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

ClusteredLightingPass::FrameBuffers ClusteredLightingPass::frameBuffers(uint32_t frameIndex) const {
    const FrameData& frame = m_frames[frameIndex];
    return {.lights = frame.lights.buffer,
            .clusterRanges = frame.clusterRanges.buffer,
            .lightIndices = frame.lightIndices.buffer};
}

void ClusteredLightingPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_pipeline.shutdown(m_device);
    for (FrameData& frame : m_frames) {
        destroyBuffer(frame.lights);
        destroyBuffer(frame.clusterRanges);
        destroyBuffer(frame.lightIndices);
        destroyBuffer(frame.statistics);
    }
    m_frames.clear();
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
