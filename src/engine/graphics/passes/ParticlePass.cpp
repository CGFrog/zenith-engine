#include "ParticlePass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr uint32_t simulationBindingCount = 11;
constexpr uint32_t drawBindingCount = 4;
constexpr uint32_t sortKeyBinding = 0;
constexpr uint32_t drawSpriteBinding = 3;
constexpr VkDeviceSize drawArgumentsSize = sizeof(VkDrawIndirectCommand);

// Mirrors the push constants of particle_sort.comp.
struct SortPushConstants {
    uint32_t step{0};
    uint32_t mergeSize{0};
    uint32_t distance{0};
};

// Mirrors the push constants of particle.vert.
struct BillboardPushConstants {
    glm::vec4 right{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 up{0.0f, 1.0f, 0.0f, 0.0f};
};

VkDescriptorSetLayoutBinding layoutBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages,
                                           uint32_t count = 1) {
    return {.binding = binding, .descriptorType = type, .descriptorCount = count, .stageFlags = stages};
}

VkWriteDescriptorSet bufferWrite(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo* buffer) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pBufferInfo = buffer};
}

VkWriteDescriptorSet imageWrite(uint32_t binding, const VkDescriptorImageInfo* images, uint32_t count = 1) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = count,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = images};
}

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

void depthBarrier(VkCommandBuffer commandBuffer, VkImage depthImage, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    // The scene color and motion targets stay attachments; later passes load
    // what this one stored.
    const VkMemoryBarrier2 attachments{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    const VkImageMemoryBarrier2 depth{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask = sourceStage,
                                      .srcAccessMask = sourceAccess,
                                      .dstStageMask = destinationStage,
                                      .dstAccessMask = destinationAccess,
                                      .oldLayout = oldLayout,
                                      .newLayout = newLayout,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = depthImage,
                                      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                           .baseMipLevel = 0,
                                                           .levelCount = 1,
                                                           .baseArrayLayer = 0,
                                                           .layerCount = 1}};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &attachments,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &depth};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

uint32_t groupCount(uint32_t items, uint32_t groupSize) {
    return (items + groupSize - 1) / groupSize;
}

VkDeviceSize sortKeyBytes(uint32_t elements) {
    return static_cast<VkDeviceSize>(elements) * sizeof(particles::SortKey);
}
} // namespace

bool ParticlePass::init(const VulkanContext& context, uint32_t frameCount, VkDescriptorSetLayout cameraLayout,
                        VkFormat colorFormat, VkFormat motionFormat, VkFormat depthFormat) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    if (!createLayouts() ||
        !createPipelines(context.pipelineCache(), cameraLayout, colorFormat, motionFormat, depthFormat)) {
        shutdown();
        return false;
    }
    m_uploads.resize(frameCount);
    for (FrameUploads& uploads : m_uploads) {
        if (!createBuffer(sizeof(particles::GpuParticleFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                          uploads.frame) ||
            !createBuffer(sizeof(particles::GpuParticleEmitter) * particles::maximumEmitters,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, uploads.emitters)) {
            shutdown();
            return false;
        }
    }
    return true;
}

bool ParticlePass::createLayouts() {
    constexpr VkShaderStageFlags compute = VK_SHADER_STAGE_COMPUTE_BIT;
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const std::array<VkDescriptorSetLayoutBinding, simulationBindingCount> simulationBindings{
        {layoutBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, compute), layoutBinding(1, storage, compute),
         layoutBinding(2, storage, compute), layoutBinding(3, storage, compute), layoutBinding(4, storage, compute),
         layoutBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, compute), layoutBinding(6, storage, compute),
         layoutBinding(7, storage, compute), layoutBinding(8, storage, compute), layoutBinding(9, storage, compute),
         layoutBinding(10, storage, compute)}};
    const VkDescriptorSetLayoutBinding sortBinding = layoutBinding(sortKeyBinding, storage, compute);
    const std::array<VkDescriptorSetLayoutBinding, drawBindingCount> drawBindings{
        {layoutBinding(0, storage, VK_SHADER_STAGE_VERTEX_BIT), layoutBinding(1, storage, VK_SHADER_STAGE_VERTEX_BIT),
         layoutBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
         layoutBinding(drawSpriteBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT,
                       particles::maximumSpriteTextures)}};
    const auto create = [this](std::span<const VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout& output,
                               const char* description) {
        const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                     .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                     .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                     .pBindings = bindings.data()};
        return checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &output), description);
    };
    if (!create(simulationBindings, m_simulationLayout, "Create particle simulation descriptor layout") ||
        !create(std::span(&sortBinding, 1), m_sortLayout, "Create particle sort descriptor layout") ||
        !create(drawBindings, m_drawLayout, "Create particle draw descriptor layout")) {
        return false;
    }
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_depthSampler), "Create particle depth sampler");
}

bool ParticlePass::createPipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout cameraLayout,
                                   VkFormat colorFormat, VkFormat motionFormat, VkFormat depthFormat) {
    ShaderModule emit(m_device);
    ShaderModule simulate(m_device);
    ShaderModule sort(m_device);
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    return emit.load(shaderRoot() / "particle_emit.comp.spv") &&
           simulate.load(shaderRoot() / "particle_simulate.comp.spv") &&
           sort.load(shaderRoot() / "particle_sort.comp.spv") && vertex.load(shaderRoot() / "particle.vert.spv") &&
           fragment.load(shaderRoot() / "particle.frag.spv") &&
           m_emitPipeline.init(m_device, m_simulationLayout, emit.handle(), pipelineCache) &&
           m_simulatePipeline.init(m_device, m_simulationLayout, simulate.handle(), pipelineCache) &&
           m_sortPipeline.init(m_device, m_sortLayout, sort.handle(), pipelineCache, sizeof(SortPushConstants)) &&
           m_drawPipeline.initParticlePipeline(m_device, colorFormat, motionFormat, depthFormat, cameraLayout,
                                               m_drawLayout, vertex.handle(), fragment.handle(), pipelineCache,
                                               sizeof(BillboardPushConstants));
}

bool ParticlePass::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& output) {
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    const VmaAllocationCreateInfo allocation =
        hostVisible ? VmaAllocationCreateInfo{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                              .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST}
                    : VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, &info),
                     "Create particle buffer")) {
        return false;
    }
    output.mapped = info.pMappedData;
    output.size = size;
    return true;
}

void ParticlePass::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void ParticlePass::destroyPool(Pool& pool) {
    destroyBuffer(pool.particles);
    destroyBuffer(pool.sortKeys);
    destroyBuffer(pool.drawArguments);
    pool = {};
}

bool ParticlePass::preparePool(uint32_t budget, uint64_t frameSerial) {
    if (budget == m_pool.budget) {
        return true;
    }
    Pool replacement{.budget = budget};
    if (budget != 0) {
        constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!createBuffer(sizeof(particles::GpuParticle) * static_cast<VkDeviceSize>(budget), storage, false,
                          replacement.particles) ||
            !createBuffer(sortKeyBytes(particles::sortElementCount(budget)), storage, false, replacement.sortKeys) ||
            !createBuffer(drawArgumentsSize, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false,
                          replacement.drawArguments)) {
            destroyPool(replacement);
            return false;
        }
    }
    // Earlier frames may still read the old pool.
    m_releaseQueue.defer(frameSerial, [this, retired = m_pool]() mutable { destroyPool(retired); });
    m_pool = replacement;
    m_scheduler.reset();
    return true;
}

void ParticlePass::collect(uint64_t completedSerial) {
    m_releaseQueue.collect(completedSerial);
}

float ParticlePass::stepSeconds() {
    constexpr float maximumStepSeconds = 0.1f;
    if (m_fixedFrameSeconds) {
        return std::clamp(*m_fixedFrameSeconds, 0.0f, maximumStepSeconds);
    }
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = m_previousStepTime ? std::chrono::duration<float>(now - *m_previousStepTime).count() : 0.0f;
    m_previousStepTime = now;
    return std::clamp(elapsed, 0.0f, maximumStepSeconds);
}

void ParticlePass::uploadFrame(uint32_t frameIndex, const particles::ParticleFramePlan& plan,
                               const ParticleEmitterSet& emitters, const ParticleSimulationFrame& frame,
                               uint32_t sortElements, float step) {
    const ClusterGrid& clusters = frame.clusters;
    const particles::GpuParticleFrame frameData{
        .viewProjection = frame.projection * frame.view,
        .lightViewProjections = frame.lightViewProjections,
        .cameraPosition = {frame.cameraPosition, step},
        .lightDirection = {frame.lightDirection, 0.0f},
        .lightRadiance = {glm::max(frame.lightRadiance, glm::vec3(0.0f)), 0.0f},
        .cascadeSplits = frame.cascadeSplits,
        .clusterDepth = {clusters.sliceScaleBias(), frame.projection[0][0], frame.projection[1][1]},
        .clusterGrid = {clusters.configuration.columns, clusters.configuration.rows, clusters.configuration.depthSlices,
                        frame.localLightCount},
        .counts = {plan.poolSlots, plan.totalSpawns, sortElements, std::min(frame.shadowCascades, 3u)},
        .ambient = {glm::max(frame.ambientRadiance, glm::vec3(0.0f)), std::max(frame.ambientScale, 0.0f)},
        .flags = {frame.environmentIrradiance ? 1u : 0u, plan.emitterTableSize, 0u, 0u},
        .irradianceVolumeMinimum = frame.irradianceVolumeMinimum,
        .irradianceVolumeCellSize = frame.irradianceVolumeCellSize,
        .irradianceVolumeCounts = frame.irradianceVolumeCounts};
    const FrameUploads& uploads = m_uploads[frameIndex];
    std::memcpy(uploads.frame.mapped, &frameData, sizeof(frameData));
    vmaFlushAllocation(m_allocator, uploads.frame.allocation, 0, sizeof(frameData));

    // Each emitter fills its table entry; unoccupied entries have no slots and
    // spawn nothing. Scheduled emitters are a subsequence of the snapshots,
    // in the same order.
    auto* packed = static_cast<particles::GpuParticleEmitter*>(uploads.emitters.mapped);
    const uint32_t tableSize = std::max(plan.emitterTableSize, 1u);
    std::fill_n(packed, tableSize, particles::GpuParticleEmitter{});
    size_t snapshot = 0;
    for (const particles::ScheduledEmitter& scheduled : plan.emitters) {
        while (emitters.emitters[snapshot].id != scheduled.id) {
            ++snapshot;
        }
        const uint32_t textureSlot = snapshot < emitters.textureSlots.size() ? emitters.textureSlots[snapshot] : 0u;
        packed[scheduled.emitterIndex] = particles::packEmitter(emitters.emitters[snapshot], scheduled, textureSlot);
    }
    vmaFlushAllocation(m_allocator, uploads.emitters.allocation, 0, sizeof(particles::GpuParticleEmitter) * tableSize);
}

void ParticlePass::pushSimulationDescriptors(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                             uint32_t frameIndex, const ParticleSimulationFrame& frame) {
    const FrameUploads& uploads = m_uploads[frameIndex];
    const VkDescriptorBufferInfo frameInfo{.buffer = uploads.frame.buffer,
                                           .range = sizeof(particles::GpuParticleFrame)};
    const VkDescriptorBufferInfo emitterInfo{.buffer = uploads.emitters.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo particleInfo{.buffer = m_pool.particles.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo keyInfo{.buffer = m_pool.sortKeys.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo argumentInfo{.buffer = m_pool.drawArguments.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorImageInfo shadowInfo{.sampler = frame.shadowSampler,
                                           .imageView = frame.shadowMap,
                                           .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightInfo{.buffer = frame.localLights, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo rangeInfo{.buffer = frame.clusterRanges, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo indexInfo{.buffer = frame.clusterLightIndices, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo irradianceInfo{.buffer = frame.environmentIrradianceBuffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo probeInfo{.buffer = frame.irradianceVolumeProbes, .range = VK_WHOLE_SIZE};
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const std::array<VkWriteDescriptorSet, simulationBindingCount> writes{
        {bufferWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &frameInfo), bufferWrite(1, storage, &emitterInfo),
         bufferWrite(2, storage, &particleInfo), bufferWrite(3, storage, &keyInfo),
         bufferWrite(4, storage, &argumentInfo), imageWrite(5, &shadowInfo), bufferWrite(6, storage, &lightInfo),
         bufferWrite(7, storage, &rangeInfo), bufferWrite(8, storage, &indexInfo),
         bufferWrite(9, storage, &irradianceInfo), bufferWrite(10, storage, &probeInfo)}};
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
}

bool ParticlePass::recordSimulation(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint64_t frameSerial,
                                    ParticleQuality quality, const ParticleEmitterSet& emitters,
                                    const ParticleSimulationFrame& frame) {
    m_drawPending = false;
    m_emitterCount = 0;
    m_capacity = 0;
    m_droppedEmitters = 0;
    if (!preparePool(particles::particleBudget(quality), frameSerial)) {
        return false;
    }
    if (m_pool.budget == 0 || frameIndex >= m_uploads.size()) {
        return true;
    }
    const float step = stepSeconds();
    const particles::ParticleFramePlan plan =
        m_scheduler.plan(emitters.emitters, m_pool.budget, step, emitters.sceneGeneration);
    m_droppedEmitters = plan.droppedEmitters;
    if (plan.emitters.empty()) {
        return true;
    }
    m_emitterCount = static_cast<uint32_t>(plan.emitters.size());
    m_capacity = plan.poolSlots;
    const uint32_t sortElements = particles::sortElementCount(plan.liveBound);
    uploadFrame(frameIndex, plan, emitters, frame, sortElements, step);

    // Earlier frames simulated, sorted, and drew from the same buffers.
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!m_pool.cleared || plan.resetPool) {
        // A zero lifetime marks a free slot.
        vkCmdFillBuffer(commandBuffer, m_pool.particles.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_pool.cleared = true;
    } else {
        // Slots of removed emitters and the new slots of added ones; the
        // other emitters keep their live particles.
        for (const particles::SlotRange& range : plan.clearRanges) {
            vkCmdFillBuffer(commandBuffer, m_pool.particles.buffer, sizeof(particles::GpuParticle) * range.first,
                            sizeof(particles::GpuParticle) * range.count, 0u);
        }
    }
    vkCmdFillBuffer(commandBuffer, m_pool.sortKeys.buffer, 0, sortKeyBytes(sortElements),
                    std::numeric_limits<uint32_t>::max());
    const VkDrawIndirectCommand noParticles{
        .vertexCount = particles::verticesPerParticle, .instanceCount = 0, .firstVertex = 0, .firstInstance = 0};
    vkCmdUpdateBuffer(commandBuffer, m_pool.drawArguments.buffer, 0, sizeof(noParticles), &noParticles);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    constexpr VkAccessFlags2 storageReadWrite =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (plan.totalSpawns > 0) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_emitPipeline.handle());
        pushSimulationDescriptors(commandBuffer, m_emitPipeline.layout(), frameIndex, frame);
        vkCmdDispatch(commandBuffer, groupCount(plan.totalSpawns, particles::simulationWorkgroupSize), 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, storageReadWrite);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_simulatePipeline.handle());
    pushSimulationDescriptors(commandBuffer, m_simulatePipeline.layout(), frameIndex, frame);
    vkCmdDispatch(commandBuffer, groupCount(plan.poolSlots, particles::simulationWorkgroupSize), 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, storageReadWrite);
    // Additive particles do not need an order, but sorting every frame keeps
    // the draw order (and so the floating-point sum) reproducible.
    recordSort(commandBuffer, sortElements);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    m_drawPending = true;
    return true;
}

void ParticlePass::recordSort(VkCommandBuffer commandBuffer, uint32_t sortElements) {
    const VkDescriptorBufferInfo keyInfo{.buffer = m_pool.sortKeys.buffer, .range = sortKeyBytes(sortElements)};
    const VkWriteDescriptorSet write = bufferWrite(sortKeyBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &keyInfo);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipeline.layout(), 0, 1, &write);
    const std::vector<particles::SortPass> plan = particles::bitonicSortPlan(sortElements);
    for (size_t index = 0; index < plan.size(); ++index) {
        if (index > 0) {
            memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        const SortPushConstants constants{.step = static_cast<uint32_t>(plan[index].step),
                                          .mergeSize = plan[index].mergeSize,
                                          .distance = plan[index].distance};
        vkCmdPushConstants(commandBuffer, m_sortPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                           &constants);
        vkCmdDispatch(commandBuffer, particles::sortDispatchGroups(sortElements), 1, 1);
    }
}

void ParticlePass::recordDraw(VkCommandBuffer commandBuffer, const ParticleDrawTargets& targets) {
    if (!m_drawPending) {
        return;
    }
    // Every render view of the frame draws the same sorted keys, so the
    // pending draw stays until the next recordSimulation.
    // Depth becomes read-only so the fragment shader can sample it for soft
    // particles while the depth test still uses it.
    depthBarrier(commandBuffer, targets.depthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const std::array<VkRenderingAttachmentInfo, 2> colors{{{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                            .imageView = targets.sceneColor,
                                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE},
                                                           {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                            .imageView = targets.motion,
                                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE}}};
    const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                          .imageView = targets.depthView,
                                          .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_NONE};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = targets.renderExtent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = static_cast<uint32_t>(colors.size()),
                                    .pColorAttachments = colors.data(),
                                    .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{.x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(targets.renderExtent.width),
                              .height = static_cast<float>(targets.renderExtent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    const VkRect2D scissor{.offset = {0, 0}, .extent = targets.renderExtent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipeline.layout(), 0, 1,
                            &targets.cameraSet, 0, nullptr);
    const VkDescriptorBufferInfo particleInfo{.buffer = m_pool.particles.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo keyInfo{.buffer = m_pool.sortKeys.buffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorImageInfo depthInfo{.sampler = m_depthSampler,
                                          .imageView = targets.depthView,
                                          .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorImageInfo, particles::maximumSpriteTextures> sprites{};
    for (size_t slot = 0; slot < sprites.size(); ++slot) {
        sprites[slot] = {.sampler = targets.spriteSampler,
                         .imageView = targets.spriteViews[slot] ? targets.spriteViews[slot] : targets.spriteViews[0],
                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const std::array<VkWriteDescriptorSet, drawBindingCount> writes{
        {bufferWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &particleInfo),
         bufferWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &keyInfo), imageWrite(2, &depthInfo),
         imageWrite(drawSpriteBinding, sprites.data(), static_cast<uint32_t>(sprites.size()))}};
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipeline.layout(), 1,
                           static_cast<uint32_t>(writes.size()), writes.data());
    // The view matrix's rows are the camera axes in world space.
    const BillboardPushConstants billboard{.right = {targets.view[0][0], targets.view[1][0], targets.view[2][0], 0.0f},
                                           .up = {targets.view[0][1], targets.view[1][1], targets.view[2][1], 0.0f}};
    vkCmdPushConstants(commandBuffer, m_drawPipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(billboard),
                       &billboard);
    vkCmdDrawIndirect(commandBuffer, m_pool.drawArguments.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
    vkCmdEndRendering(commandBuffer);
    depthBarrier(commandBuffer, targets.depthImage, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

uint64_t ParticlePass::allocatedBytes() const {
    uint64_t bytes = m_pool.particles.size + m_pool.sortKeys.size + m_pool.drawArguments.size;
    for (const FrameUploads& uploads : m_uploads) {
        bytes += uploads.frame.size + uploads.emitters.size;
    }
    return bytes;
}

void ParticlePass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_releaseQueue.collect(std::numeric_limits<uint64_t>::max());
    destroyPool(m_pool);
    for (FrameUploads& uploads : m_uploads) {
        destroyBuffer(uploads.frame);
        destroyBuffer(uploads.emitters);
    }
    m_uploads.clear();
    m_emitPipeline.shutdown(m_device);
    m_simulatePipeline.shutdown(m_device);
    m_sortPipeline.shutdown(m_device);
    m_drawPipeline.shutdown(m_device);
    if (m_depthSampler) {
        vkDestroySampler(m_device, m_depthSampler, nullptr);
    }
    for (VkDescriptorSetLayout* layout : {&m_simulationLayout, &m_sortLayout, &m_drawLayout}) {
        if (*layout) {
            vkDestroyDescriptorSetLayout(m_device, *layout, nullptr);
        }
        *layout = VK_NULL_HANDLE;
    }
    m_depthSampler = VK_NULL_HANDLE;
    m_scheduler.reset();
    m_drawPending = false;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
