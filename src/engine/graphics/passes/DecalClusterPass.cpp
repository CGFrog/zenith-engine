#include "DecalClusterPass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
constexpr uint32_t workgroupSize = 64;
constexpr uint32_t bindingCount = 3;
// Per cluster: a count followed by maximumDecalsPerCluster indices.
constexpr uint32_t clusterListStride = 1 + maximumDecalsPerCluster;
static_assert(clusterListStride == 32, "update DECAL_CLUSTER_STRIDE in mesh.frag and decal_clusters.comp");

// Byte layout of Decals in shaders/mesh.frag (std430).
constexpr VkDeviceSize recordsHeaderBytes = sizeof(glm::uvec4);
constexpr VkDeviceSize recordsBytes = recordsHeaderBytes + sizeof(GpuDecal) * maximumDecals;

// Mirrors DecalClusterSettings in shaders/decal_clusters.comp.
struct DecalClusterSettings {
    glm::vec4 projection{0.0f};
    glm::vec4 depth{0.0f};
    glm::uvec4 grid{0u};
};

// Mirrors Statistics in shaders/decal_clusters.comp.
struct StatisticsRecord {
    uint32_t maximumDecalsInCluster{0};
    uint32_t discardedAssignments{0};
};

glm::vec4 matrixRow(const glm::mat4& matrix, int row) {
    return {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
}
} // namespace

GpuDecal packDecal(const DecalProjector& decal, const DecalTextureSlots& slots, const glm::mat4& view) {
    const ClusterBounds bounds = decalViewBounds(decal, view);
    const uint32_t flags = (slots.albedo ? decalAlbedoFlag : 0u) | (slots.normal ? decalNormalFlag : 0u);
    return {.worldToDecal = {matrixRow(decal.worldToDecal, 0), matrixRow(decal.worldToDecal, 1),
                             matrixRow(decal.worldToDecal, 2)},
            .tintOpacity = {decal.tint, decal.opacity},
            .surface = {decal.roughness, decal.roughnessStrength, decal.metallic, decal.metallicStrength},
            .blend = {decal.baseColorStrength, decal.normalStrength, decal.angleFadeScale, decal.angleFadeBias},
            .textures = {slots.albedo.value_or(0u), slots.normal.value_or(0u), flags, 0u},
            .viewMinimum = {bounds.minimum, 0.0f},
            .viewMaximum = {bounds.maximum, 0.0f}};
}

bool DecalClusterPass::init(const VulkanContext& context, uint32_t frameCount,
                            const ClusterGridConfiguration& configuration) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_configuration = makeClusterGrid(configuration, glm::mat4(1.0f), {1, 1}, 1.0f, 2.0f).configuration;
    ShaderModule shader(m_device);
    if (!createDescriptors(frameCount) || !shader.load(shaderRoot() / "decal_clusters.comp.spv") ||
        !m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(DecalClusterSettings))) {
        shutdown();
        return false;
    }
    return true;
}

bool DecalClusterPass::createDescriptors(uint32_t frameCount) {
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
    const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .descriptorCount = frameCount * bindingCount};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = 1,
                                              .pPoolSizes = &poolSize};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
                     "Create decal-cluster descriptor layout") ||
        !checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool),
                     "Create decal-cluster descriptor pool")) {
        return false;
    }
    m_frames.resize(frameCount);
    for (FrameData& frame : m_frames) {
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = m_pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &m_layout};
        if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, &frame.descriptor),
                         "Allocate decal-cluster descriptor set") ||
            !createFrameBuffers(frame)) {
            return false;
        }
        const std::array<VkDescriptorBufferInfo, bindingCount> buffers{
            {{.buffer = frame.records.buffer, .range = VK_WHOLE_SIZE},
             {.buffer = frame.clusterLists.buffer, .range = VK_WHOLE_SIZE},
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

bool DecalClusterPass::createFrameBuffers(FrameData& frame) {
    const uint32_t clusterCount = m_configuration.columns * m_configuration.rows * m_configuration.depthSlices;
    if (!createBuffer(recordsBytes, BufferAccess::HostWrite, frame.records) ||
        !createBuffer(sizeof(uint32_t) * clusterListStride * clusterCount, BufferAccess::Device, frame.clusterLists) ||
        !createBuffer(sizeof(StatisticsRecord), BufferAccess::HostReadback, frame.statistics)) {
        return false;
    }
    // A zero header keeps fragment shaders from reading the unwritten lists.
    std::memset(frame.records.mapped, 0, recordsHeaderBytes);
    vmaFlushAllocation(m_allocator, frame.records.allocation, 0, recordsHeaderBytes);
    std::memset(frame.statistics.mapped, 0, sizeof(StatisticsRecord));
    vmaFlushAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
    return true;
}

bool DecalClusterPass::createBuffer(VkDeviceSize size, BufferAccess access, Buffer& output) {
    const VkBufferCreateInfo buffer{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    // Fragment shaders read the records, so they prefer device-local
    // host-visible memory; the small statistics record is read back by the host.
    if (access == BufferAccess::HostWrite) {
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else if (access == BufferAccess::HostReadback) {
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
    }
    VmaAllocationInfo info{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, &info),
                     "Create decal-cluster buffer")) {
        return false;
    }
    output.mapped = info.pMappedData;
    output.size = size;
    return true;
}

void DecalClusterPass::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

uint32_t DecalClusterPass::beginFrame(uint32_t frameIndex, std::span<const GpuDecal> decals,
                                      DecalClusterStatistics& retired) {
    FrameData& frame = m_frames[frameIndex];
    // The caller waited for this slot, so its last assignment is complete.
    retired = {};
    if (frame.statisticsPending) {
        vmaInvalidateAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
        StatisticsRecord record;
        std::memcpy(&record, frame.statistics.mapped, sizeof(record));
        retired = {.maximumDecalsInCluster = record.maximumDecalsInCluster,
                   .discardedAssignments = record.discardedAssignments};
        std::memset(frame.statistics.mapped, 0, sizeof(StatisticsRecord));
        vmaFlushAllocation(m_allocator, frame.statistics.allocation, 0, sizeof(StatisticsRecord));
        frame.statisticsPending = false;
    }
    frame.decalCount = static_cast<uint32_t>(std::min<size_t>(decals.size(), maximumDecals));
    auto* bytes = static_cast<std::byte*>(frame.records.mapped);
    const glm::uvec4 header{frame.decalCount, 0u, 0u, 0u};
    std::memcpy(bytes, &header, sizeof(header));
    if (frame.decalCount > 0) {
        std::memcpy(bytes + recordsHeaderBytes, decals.data(), sizeof(GpuDecal) * frame.decalCount);
    }
    vmaFlushAllocation(m_allocator, frame.records.allocation, 0,
                       recordsHeaderBytes + sizeof(GpuDecal) * frame.decalCount);
    return frame.decalCount;
}

void DecalClusterPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const ClusterGrid& grid) {
    FrameData& frame = m_frames[frameIndex];
    if (frame.decalCount == 0) {
        return;
    }
    // The list buffer is sized for the configured grid, which the caller's
    // grid must have been built from.
    const ClusterGridConfiguration& gridSize = m_configuration;
    const DecalClusterSettings settings{
        .projection = {grid.projectionScale, grid.projectionOffset},
        .depth = {grid.nearDepth, grid.sliceEndDepth, grid.farDepth, 0.0f},
        .grid = {gridSize.columns, gridSize.rows, gridSize.depthSlices, frame.decalCount}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1, &frame.descriptor,
                            0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    const uint32_t clusterCount = gridSize.columns * gridSize.rows * gridSize.depthSlices;
    vkCmdDispatch(commandBuffer, (clusterCount + workgroupSize - 1) / workgroupSize, 1, 1);
    frame.statisticsPending = true;
    // Lists feed this frame's fragment shading; statistics are read by the
    // host once the frame's fence signals.
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                   .dstStageMask =
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                                   .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

DecalClusterPass::FrameBuffers DecalClusterPass::frameBuffers(uint32_t frameIndex) const {
    const FrameData& frame = m_frames[frameIndex];
    return {.records = frame.records.buffer, .clusterLists = frame.clusterLists.buffer};
}

uint64_t DecalClusterPass::allocatedBytes() const {
    uint64_t bytes = 0;
    for (const FrameData& frame : m_frames) {
        bytes += frame.records.size + frame.clusterLists.size + frame.statistics.size;
    }
    return bytes;
}

void DecalClusterPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_pipeline.shutdown(m_device);
    for (FrameData& frame : m_frames) {
        destroyBuffer(frame.records);
        destroyBuffer(frame.clusterLists);
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
