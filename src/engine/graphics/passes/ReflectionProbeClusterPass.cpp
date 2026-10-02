#include "ReflectionProbeClusterPass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>

namespace {
constexpr uint32_t workgroupSize = 64;
constexpr VkDeviceSize irradianceCoefficients = 9;
constexpr VkDeviceSize irradianceBytes = sizeof(glm::vec4) * irradianceCoefficients;

// Byte offsets of ReflectionProbes in shaders/mesh.frag (std430).
constexpr VkDeviceSize headerBytes = sizeof(glm::uvec4);
constexpr VkDeviceSize recordsOffset = headerBytes;
constexpr VkDeviceSize irradianceOffset = recordsOffset + sizeof(GpuReflectionProbe) * maximumResidentReflectionProbes;
constexpr VkDeviceSize clusterMasksOffset = irradianceOffset + irradianceBytes * maximumResidentReflectionProbes;
static_assert(irradianceOffset == 656 && clusterMasksOffset == 1808, "update ReflectionProbes in mesh.frag");

// Header and records, uploaded with vkCmdUpdateBuffer.
struct ProbeUpload {
    glm::uvec4 header{0u};
    std::array<GpuReflectionProbe, maximumResidentReflectionProbes> records{};
};

static_assert(sizeof(ProbeUpload) == irradianceOffset);

// Mirrors ProbeClusterSettings in shaders/reflection_probe_clusters.comp.
struct ProbeClusterSettings {
    glm::vec4 projection{0.0f};
    glm::vec4 depth{0.0f};
    glm::uvec4 grid{0u};
};

void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                   VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = sourceStage,
                                   .srcAccessMask = sourceAccess,
                                   .dstStageMask = destinationStage,
                                   .dstAccessMask = destinationAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

GpuReflectionProbe packReflectionProbe(const ReflectionProbeVolume& probe, uint32_t cacheSlot, const glm::mat4& view) {
    const ClusterBounds bounds = reflectionProbeViewBounds(probe, view);
    return {.centerBlend = {probe.center, probe.blendDistance},
            .halfExtents = {probe.halfExtents, 0.0f},
            .parameters = {probe.priority, static_cast<int32_t>(cacheSlot), 0, 0},
            .viewMinimum = {bounds.minimum, 0.0f},
            .viewMaximum = {bounds.maximum, 0.0f}};
}

bool ReflectionProbeClusterPass::init(const VulkanContext& context, uint32_t frameCount,
                                      const ClusterGridConfiguration& configuration) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_configuration = makeClusterGrid(configuration, glm::mat4(1.0f), {1, 1}, 1.0f, 2.0f).configuration;
    ShaderModule shader(m_device);
    if (!createFrames(frameCount) || !shader.load(shaderRoot() / "reflection_probe_clusters.comp.spv") ||
        !m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(ProbeClusterSettings))) {
        shutdown();
        return false;
    }
    return true;
}

bool ReflectionProbeClusterPass::createFrames(uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding binding{.binding = 0,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               .descriptorCount = 1,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = frameCount};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = 1,
                                              .pPoolSizes = &poolSize};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
                     "Create reflection-probe cluster layout") ||
        !checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool),
                     "Create reflection-probe cluster pool")) {
        return false;
    }
    const uint32_t clusterCount = m_configuration.columns * m_configuration.rows * m_configuration.depthSlices;
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = clusterMasksOffset + sizeof(uint32_t) * clusterCount,
                                        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    m_frames.resize(frameCount);
    for (FrameData& frame : m_frames) {
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = m_pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &m_layout};
        if (!checkVulkan(
                vmaCreateBuffer(m_allocator, &bufferInfo, &allocation, &frame.buffer, &frame.allocation, nullptr),
                "Create reflection-probe buffer") ||
            !checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, &frame.descriptor),
                         "Allocate reflection-probe cluster set")) {
            return false;
        }
        const VkDescriptorBufferInfo bufferDescriptor{.buffer = frame.buffer, .range = VK_WHOLE_SIZE};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = frame.descriptor,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &bufferDescriptor};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
    return true;
}

void ReflectionProbeClusterPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                        std::span<const GpuReflectionProbe> probes, VkBuffer irradianceSource,
                                        const ClusterGrid& grid) {
    const uint32_t probeCount = static_cast<uint32_t>(std::min<size_t>(probes.size(), maximumResidentReflectionProbes));
    if (probeCount == 0 || frameIndex >= m_frames.size() || irradianceSource == VK_NULL_HANDLE) {
        return;
    }
    const FrameData& frame = m_frames[frameIndex];
    // The frame's previous use of its buffer retired before this slot was
    // reused, so only this frame's own writes need ordering.
    ProbeUpload upload{.header = {probeCount, 0u, 0u, 0u}};
    std::copy_n(probes.begin(), probeCount, upload.records.begin());
    vkCmdUpdateBuffer(commandBuffer, frame.buffer, 0, recordsOffset + sizeof(GpuReflectionProbe) * probeCount, &upload);
    std::array<VkBufferCopy, maximumResidentReflectionProbes> copies{};
    for (uint32_t index = 0; index < probeCount; ++index) {
        const auto slot = static_cast<VkDeviceSize>(std::max(probes[index].parameters.y, 0));
        copies[index] = {.srcOffset = irradianceBytes * slot,
                         .dstOffset = irradianceOffset + irradianceBytes * index,
                         .size = irradianceBytes};
    }
    vkCmdCopyBuffer(commandBuffer, irradianceSource, frame.buffer, probeCount, copies.data());
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    const ClusterGridConfiguration& gridSize = m_configuration;
    const ProbeClusterSettings settings{.projection = {grid.projectionScale, grid.projectionOffset},
                                        .depth = {grid.nearDepth, grid.sliceEndDepth, grid.farDepth, 0.0f},
                                        .grid = {gridSize.columns, gridSize.rows, gridSize.depthSlices, probeCount}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1, &frame.descriptor,
                            0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    const uint32_t clusterCount = gridSize.columns * gridSize.rows * gridSize.depthSlices;
    vkCmdDispatch(commandBuffer, (clusterCount + workgroupSize - 1) / workgroupSize, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void ReflectionProbeClusterPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_pipeline.shutdown(m_device);
    for (FrameData& frame : m_frames) {
        if (frame.buffer) {
            vmaDestroyBuffer(m_allocator, frame.buffer, frame.allocation);
        }
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
