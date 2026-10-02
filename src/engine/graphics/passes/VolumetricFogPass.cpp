#include "VolumetricFogPass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
// Mirrors local_size in the fog compute shaders.
constexpr uint32_t workgroupSize = 8;
constexpr uint32_t injectBindingCount = 9;
// Halton points cycled by temporally reprojected qualities.
constexpr uint32_t jitterSequenceLength = 16;

// Mirrors FogFrame in shaders/volumetric_fog_inject.comp (std140).
struct InjectUniforms {
    glm::mat4 inverseView{1.0f};
    glm::mat4 previousViewProjection{1.0f};
    std::array<glm::mat4, 3> lightViewProjections{};
    glm::vec4 cameraPosition{0.0f};
    // xyz: toward the light.
    glm::vec4 lightDirection{0.0f};
    glm::vec4 lightRadiance{0.0f};
    glm::vec4 cascadeSplits{0.0f};
    // xy: projection scale, zw: offset (see ClusterGrid).
    glm::vec4 projection{0.0f};
    // x, y: slice log scale and bias; z: near depth; w: depth where the volume ends.
    glm::vec4 slices{0.0f};
    // x: history valid; y, z: previous frame's slice scale and bias; w: current-frame weight.
    glm::vec4 history{0.0f};
    // xyz: sample position inside each froxel, in [0, 1).
    glm::vec4 jitter{0.5f};
    // xyz: froxel counts; w: shadow cascades.
    glm::uvec4 volume{0u};
    // xyz: froxels per cluster; w: local light count.
    glm::uvec4 clusters{0u};
    // xyz: cluster columns, rows, depth slices; w: 1 to read ambient from the irradiance SH.
    glm::uvec4 clusterGrid{0u};
    // rgb: single-scattering albedo; w: Henyey-Greenstein anisotropy.
    glm::vec4 albedoAnisotropy{0.0f};
    // x: density; y: height falloff; z: base height; w: noise amount.
    glm::vec4 density{0.0f};
    // x: noise cell size; w: ambient scale.
    glm::vec4 noiseAmbient{0.0f};
    glm::vec4 ambientRadiance{0.0f};
    // Irradiance volume: see VolumetricFogFrame.
    glm::vec4 irradianceVolumeMinimum{0.0f};
    glm::vec4 irradianceVolumeCellSize{1.0f};
    glm::uvec4 irradianceVolumeCounts{0u};
};

// Mirrors Settings in shaders/volumetric_fog_integrate.comp.
struct IntegrateSettings {
    glm::uvec4 volume{0u};
    glm::vec4 projection{0.0f};
    // x, y: slice log scale and bias; z: near depth; w: depth where the volume ends.
    glm::vec4 slices{0.0f};
};

static_assert(sizeof(IntegrateSettings) <= 128, "Fog integration push constants must fit the guaranteed minimum");

float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    for (uint32_t value = index; value > 0; value /= base) {
        result += fraction * static_cast<float>(value % base);
        fraction /= static_cast<float>(base);
    }
    return result;
}

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type) {
    return {
        .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

VkWriteDescriptorSet imageWrite(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = image};
}

VkWriteDescriptorSet bufferWrite(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo* buffer) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pBufferInfo = buffer};
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

uint32_t groupCount(uint32_t texels) {
    return (texels + workgroupSize - 1) / workgroupSize;
}

constexpr VkImageSubresourceRange colorRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
} // namespace

bool VolumetricFogPass::init(const VulkanContext& context, uint32_t frameCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context.pDevice(), volumeFormat, &properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    m_supported = (properties.optimalTilingFeatures & required) == required;
    if (!m_supported) {
        std::cerr << "Volumetric fog disabled: RGBA16F storage images are unsupported\n";
    }
    if (!createLayouts() || (m_supported && !createPipelines(context.pipelineCache())) ||
        !createUniformBuffers(frameCount) || !createVolumes(VolumetricFogQuality::Off, m_volumes)) {
        shutdown();
        return false;
    }
    return true;
}

bool VolumetricFogPass::createLayouts() {
    const std::array<VkDescriptorSetLayoutBinding, injectBindingCount> injectBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
         computeBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), computeBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), computeBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)}};
    const std::array<VkDescriptorSetLayoutBinding, 2> integrateBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}};
    VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                           .bindingCount = static_cast<uint32_t>(injectBindings.size()),
                                           .pBindings = injectBindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_injectLayout),
                     "Create fog injection descriptor layout")) {
        return false;
    }
    layout.bindingCount = static_cast<uint32_t>(integrateBindings.size());
    layout.pBindings = integrateBindings.data();
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_integrateLayout),
                     "Create fog integration descriptor layout")) {
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
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create fog volume sampler");
}

bool VolumetricFogPass::createPipelines(VkPipelineCache pipelineCache) {
    ShaderModule inject(m_device);
    ShaderModule integrate(m_device);
    return inject.load(shaderRoot() / "volumetric_fog_inject.comp.spv") &&
           integrate.load(shaderRoot() / "volumetric_fog_integrate.comp.spv") &&
           m_injectPipeline.init(m_device, m_injectLayout, inject.handle(), pipelineCache) &&
           m_integratePipeline.init(m_device, m_integrateLayout, integrate.handle(), pipelineCache,
                                    sizeof(IntegrateSettings));
}

bool VolumetricFogPass::createUniformBuffers(uint32_t frameCount) {
    m_uniforms.resize(frameCount);
    for (UniformBuffer& uniform : m_uniforms) {
        const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = sizeof(InjectUniforms),
                                        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                                 .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        if (!checkVulkan(
                vmaCreateBuffer(m_allocator, &buffer, &allocation, &uniform.buffer, &uniform.allocation, &info),
                "Create fog uniform buffer")) {
            return false;
        }
        uniform.mapped = info.pMappedData;
    }
    return true;
}

bool VolumetricFogPass::createImage(const glm::uvec3& extent, VkImageUsageFlags usage, Image& output) {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_3D,
                                  .format = volumeFormat,
                                  .extent = {extent.x, extent.y, extent.z},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = usage,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &output.image, &output.allocation, nullptr),
                     "Create fog volume")) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = output.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_3D,
                                     .format = volumeFormat,
                                     .subresourceRange = colorRange};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.view), "Create fog volume view");
}

bool VolumetricFogPass::createVolumes(VolumetricFogQuality quality, Volumes& output) {
    const glm::uvec3 subdivision = volumetric_fog::froxelSubdivision(quality);
    const ClusterGridConfiguration clusters;
    output = {};
    output.quality = quality;
    output.subdivision = subdivision;
    output.extent = quality == VolumetricFogQuality::Off
                        ? glm::uvec3(1u)
                        : glm::uvec3(clusters.columns, clusters.rows, clusters.depthSlices) * subdivision;
    output.generation = m_nextGeneration++;
    // Without storage support only the cleared integrated volume exists.
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (m_supported ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    if (!createImage(output.extent, usage, output.integrated)) {
        return false;
    }
    if (!m_supported) {
        return true;
    }
    return createImage(output.extent, usage, output.scattering[0]) &&
           createImage(output.extent, usage, output.scattering[1]);
}

bool VolumetricFogPass::prepare(VolumetricFogQuality quality, uint64_t frameSerial) {
    const VolumetricFogQuality supportedQuality = m_supported ? quality : VolumetricFogQuality::Off;
    if (supportedQuality == m_volumes.quality) {
        return true;
    }
    Volumes replacement;
    if (!createVolumes(supportedQuality, replacement)) {
        destroyVolumes(replacement);
        return false;
    }
    // Frames up to the previous one may still read the old volumes.
    m_releaseQueue.defer(frameSerial, [this, retired = m_volumes]() mutable { destroyVolumes(retired); });
    m_volumes = replacement;
    return true;
}

void VolumetricFogPass::collect(uint64_t completedSerial) {
    m_releaseQueue.collect(completedSerial);
}

void VolumetricFogPass::initializeVolumes(VkCommandBuffer commandBuffer) {
    std::array<VkImageMemoryBarrier2, 3> barriers{};
    uint32_t barrierCount = 0;
    for (const Image* image : {&m_volumes.integrated, &m_volumes.scattering[0], &m_volumes.scattering[1]}) {
        if (image->image == VK_NULL_HANDLE) {
            continue;
        }
        barriers[barrierCount++] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .srcAccessMask = 0,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = volumeLayout,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image->image,
                                    .subresourceRange = colorRange};
    }
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = barrierCount,
                                      .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    const VkClearColorValue noFog{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    const VkClearColorValue emptyMedium{.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    vkCmdClearColorImage(commandBuffer, m_volumes.integrated.image, volumeLayout, &noFog, 1, &colorRange);
    for (const Image& scattering : m_volumes.scattering) {
        if (scattering.image != VK_NULL_HANDLE) {
            vkCmdClearColorImage(commandBuffer, scattering.image, volumeLayout, &emptyMedium, 1, &colorRange);
        }
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_volumes.contents = Contents::Cleared;
    m_volumes.historyValid = false;
}

void VolumetricFogPass::recordCleared(VkCommandBuffer commandBuffer) {
    if (m_volumes.contents == Contents::Undefined) {
        initializeVolumes(commandBuffer);
        return;
    }
    if (m_volumes.contents == Contents::Cleared) {
        return;
    }
    // Earlier frames' shading may still read the volume.
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue noFog{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(commandBuffer, m_volumes.integrated.image, volumeLayout, &noFog, 1, &colorRange);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_volumes.contents = Contents::Cleared;
    m_volumes.historyValid = false;
}

void VolumetricFogPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const VolumetricFogFrame& frame) {
    if (!m_supported || m_volumes.quality == VolumetricFogQuality::Off || frameIndex >= m_uniforms.size()) {
        recordCleared(commandBuffer);
        return;
    }
    if (m_volumes.contents == Contents::Undefined) {
        initializeVolumes(commandBuffer);
    }
    // Earlier frames sampled the integrated volume in fragment shaders and
    // wrote or read the scattering volumes in compute; all stay in GENERAL.
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    recordInject(commandBuffer, frameIndex, frame);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    recordIntegrate(commandBuffer, frame);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_volumes.contents = Contents::Fog;
    m_volumes.historyValid = true;
    m_volumes.previousSliceScaleBias = frame.grid.sliceScaleBias();
    m_volumes.current ^= 1u;
    ++m_frameNumber;
}

void VolumetricFogPass::recordInject(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                     const VolumetricFogFrame& frame) {
    const bool temporal = volumetric_fog::temporalReprojection(m_volumes.quality);
    const bool historyValid = temporal && frame.historyValid && m_volumes.historyValid;
    const uint32_t jitterIndex = m_frameNumber % jitterSequenceLength + 1;
    const glm::vec3 jitter =
        temporal ? glm::vec3(halton(jitterIndex, 2), halton(jitterIndex, 3), halton(jitterIndex, 5)) : glm::vec3(0.5f);
    const ClusterGrid& clusters = frame.grid.clusters;
    const glm::vec2 sliceScaleBias = frame.grid.sliceScaleBias();
    const VolumetricFogComponent& medium = frame.medium;
    const InjectUniforms uniforms{
        .inverseView = frame.inverseView,
        .previousViewProjection = frame.previousViewProjection,
        .lightViewProjections = frame.lightViewProjections,
        .cameraPosition = {frame.cameraPosition, 1.0f},
        .lightDirection = {frame.lightDirection, 0.0f},
        .lightRadiance = {frame.lightRadiance, 0.0f},
        .cascadeSplits = frame.cascadeSplits,
        .projection = {clusters.projectionScale, clusters.projectionOffset},
        .slices = {sliceScaleBias, clusters.nearDepth, clusters.sliceEndDepth},
        .history = {historyValid ? 1.0f : 0.0f, m_volumes.previousSliceScaleBias, volumetric_fog::currentFrameWeight},
        .jitter = {jitter, 0.0f},
        .volume = {m_volumes.extent, std::min(frame.shadowCascades, 3u)},
        .clusters = {m_volumes.subdivision, frame.localLightCount},
        .clusterGrid = {clusters.configuration.columns, clusters.configuration.rows, clusters.configuration.depthSlices,
                        frame.environmentIrradiance ? 1u : 0u},
        .albedoAnisotropy = {glm::max(medium.albedo, glm::vec3(0.0f)), std::clamp(medium.anisotropy, -0.99f, 0.99f)},
        .density = {std::max(medium.density, 0.0f), std::max(medium.heightFalloff, 0.0f), medium.baseHeight,
                    std::clamp(medium.noiseAmount, 0.0f, 1.0f)},
        .noiseAmbient = {std::max(medium.noiseScale, 0.001f), 0.0f, 0.0f, std::max(frame.ambientScale, 0.0f)},
        .ambientRadiance = {glm::max(frame.ambientRadiance, glm::vec3(0.0f)), 0.0f},
        .irradianceVolumeMinimum = frame.irradianceVolumeMinimum,
        .irradianceVolumeCellSize = frame.irradianceVolumeCellSize,
        .irradianceVolumeCounts = frame.irradianceVolumeCounts};
    const UniformBuffer& uniform = m_uniforms[frameIndex];
    std::memcpy(uniform.mapped, &uniforms, sizeof(uniforms));
    vmaFlushAllocation(m_allocator, uniform.allocation, 0, sizeof(uniforms));

    const Image& output = m_volumes.scattering[m_volumes.current];
    const Image& history = m_volumes.scattering[m_volumes.current ^ 1u];
    const VkDescriptorBufferInfo uniformInfo{.buffer = uniform.buffer, .range = sizeof(InjectUniforms)};
    const VkDescriptorImageInfo shadowInfo{.sampler = frame.shadowSampler,
                                           .imageView = frame.shadowMap,
                                           .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightInfo{.buffer = frame.localLights, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo rangeInfo{.buffer = frame.clusterRanges, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo indexInfo{.buffer = frame.clusterLightIndices, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo irradianceInfo{.buffer = frame.environmentIrradianceBuffer, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo probeInfo{.buffer = frame.irradianceVolumeProbes, .range = VK_WHOLE_SIZE};
    const VkDescriptorImageInfo historyInfo{
        .sampler = m_sampler, .imageView = history.view, .imageLayout = volumeLayout};
    const VkDescriptorImageInfo outputInfo{.imageView = output.view, .imageLayout = volumeLayout};
    const std::array<VkWriteDescriptorSet, injectBindingCount> writes{
        {bufferWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformInfo),
         imageWrite(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowInfo),
         bufferWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lightInfo),
         bufferWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &rangeInfo),
         bufferWrite(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &indexInfo),
         bufferWrite(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &irradianceInfo),
         imageWrite(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyInfo),
         imageWrite(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo),
         bufferWrite(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &probeInfo)}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_injectPipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_injectPipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdDispatch(commandBuffer, groupCount(m_volumes.extent.x), groupCount(m_volumes.extent.y), m_volumes.extent.z);
}

void VolumetricFogPass::recordIntegrate(VkCommandBuffer commandBuffer, const VolumetricFogFrame& frame) {
    const ClusterGrid& clusters = frame.grid.clusters;
    const IntegrateSettings settings{
        .volume = {m_volumes.extent, 0u},
        .projection = {clusters.projectionScale, clusters.projectionOffset},
        .slices = {frame.grid.sliceScaleBias(), clusters.nearDepth, clusters.sliceEndDepth}};
    const VkDescriptorImageInfo scattering{.imageView = m_volumes.scattering[m_volumes.current].view,
                                           .imageLayout = volumeLayout};
    const VkDescriptorImageInfo integrated{.imageView = m_volumes.integrated.view, .imageLayout = volumeLayout};
    const std::array<VkWriteDescriptorSet, 2> writes{{imageWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &scattering),
                                                      imageWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &integrated)}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_integratePipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_integratePipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdPushConstants(commandBuffer, m_integratePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, groupCount(m_volumes.extent.x), groupCount(m_volumes.extent.y), 1);
}

uint64_t VolumetricFogPass::allocatedBytes() const {
    const glm::u64vec3 extent(m_volumes.extent);
    const uint64_t volumeBytes = extent.x * extent.y * extent.z * sizeof(uint16_t) * 4;
    const uint64_t volumes = m_volumes.scattering[0].image != VK_NULL_HANDLE ? 3u : 1u;
    return volumeBytes * volumes;
}

void VolumetricFogPass::destroyImage(Image& image) {
    if (image.view) {
        vkDestroyImageView(m_device, image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
    image = {};
}

void VolumetricFogPass::destroyVolumes(Volumes& volumes) {
    destroyImage(volumes.integrated);
    for (Image& scattering : volumes.scattering) {
        destroyImage(scattering);
    }
}

void VolumetricFogPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_releaseQueue.collect(std::numeric_limits<uint64_t>::max());
    destroyVolumes(m_volumes);
    m_volumes = {};
    for (UniformBuffer& uniform : m_uniforms) {
        if (uniform.buffer) {
            vmaDestroyBuffer(m_allocator, uniform.buffer, uniform.allocation);
        }
    }
    m_uniforms.clear();
    m_injectPipeline.shutdown(m_device);
    m_integratePipeline.shutdown(m_device);
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_injectLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_injectLayout, nullptr);
    }
    if (m_integrateLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_integrateLayout, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_injectLayout = VK_NULL_HANDLE;
    m_integrateLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_supported = false;
    m_frameNumber = 0;
}
