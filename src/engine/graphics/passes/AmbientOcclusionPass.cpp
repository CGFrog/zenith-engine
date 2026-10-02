#include "AmbientOcclusionPass.hpp"

#include "../DepthPyramidLayout.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace {
constexpr uint32_t workgroupSize = 8;

// Mirrors Settings in shaders/gtao.comp.
struct GtaoSettings {
    glm::mat4 inverseProjection{1.0f};
    glm::ivec2 dispatchExtent{0};
    glm::ivec2 renderExtent{0};
    float resolutionScale{1.0f};
    float radius{0.0f};
    float falloffFraction{0.0f};
    float projectionScale{1.0f};
    uint32_t noiseFrame{0};
    uint32_t sliceCount{0};
    uint32_t stepsPerSide{0};
    uint32_t horizonLevel{0};
};

static_assert(sizeof(GtaoSettings) <= 128, "GTAO push constants must fit the guaranteed minimum");

// Mirrors Settings in shaders/gtao_denoise.comp.
struct DenoiseSettings {
    glm::ivec2 dispatchExtent{0};
    float occlusionPower{1.0f};
};

// Mirrors Settings in shaders/gtao_upsample.comp.
struct UpsampleSettings {
    glm::ivec2 dispatchExtent{0};
    glm::ivec2 renderExtent{0};
    glm::ivec2 rawExtent{0};
    glm::vec2 viewDepthTerms{0.0f};
    float occlusionPower{1.0f};
};

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type) {
    return {
        .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

VkWriteDescriptorSet pushWrite(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = image};
}

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStage,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStage,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
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

glm::uvec2 toVector(VkExtent2D extent) {
    return {extent.width, extent.height};
}

uint32_t groupCount(uint32_t texels) {
    return (texels + workgroupSize - 1) / workgroupSize;
}

bool supportsStorage(const VulkanContext& context, VkFormat format) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context.pDevice(), format, &properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (properties.optimalTilingFeatures & required) == required;
}
} // namespace

float AmbientOcclusionPass::resolutionScale(AmbientOcclusionQuality /*quality*/) {
    return 1.0f;
}

bool AmbientOcclusionPass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D storageExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    // Without storage support the pass still owns cleared images, so readers
    // keep a valid binding; occlusion is then always unoccluded.
    m_computeSupported = supportsStorage(context, occlusionFormat) && supportsStorage(context, rawFormat);
    if (!m_computeSupported) {
        std::cerr << "Ambient occlusion disabled: R8_UNORM or R16G16_SFLOAT storage images are unsupported\n";
    }
    m_frames.resize(frameCount);
    if (!createLayouts() || (m_computeSupported && !createPipelines(context.pipelineCache())) ||
        !createImages(storageExtent)) {
        shutdown();
        return false;
    }
    return true;
}

bool AmbientOcclusionPass::createLayouts() {
    const std::array<VkDescriptorSetLayoutBinding, 3> gtaoBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}};
    const std::array<VkDescriptorSetLayoutBinding, 2> denoiseBindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}};
    VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                           .bindingCount = static_cast<uint32_t>(gtaoBindings.size()),
                                           .pBindings = gtaoBindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_gtaoLayout),
                     "Create GTAO descriptor layout")) {
        return false;
    }
    layout.bindingCount = static_cast<uint32_t>(denoiseBindings.size());
    layout.pBindings = denoiseBindings.data();
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_denoiseLayout),
                     "Create GTAO denoise descriptor layout")) {
        return false;
    }
    // The upsample binds like GTAO: raw occlusion, depth pyramid, output.
    layout.bindingCount = static_cast<uint32_t>(gtaoBindings.size());
    layout.pBindings = gtaoBindings.data();
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_upsampleLayout),
                     "Create GTAO upsample descriptor layout")) {
        return false;
    }
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = 0.0f};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_pointSampler), "Create GTAO point sampler")) {
        return false;
    }
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_linearSampler),
                       "Create ambient-occlusion sampler");
}

bool AmbientOcclusionPass::createPipelines(VkPipelineCache pipelineCache) {
    ShaderModule gtao(m_device);
    ShaderModule denoise(m_device);
    ShaderModule upsample(m_device);
    return gtao.load(shaderRoot() / "gtao.comp.spv") && denoise.load(shaderRoot() / "gtao_denoise.comp.spv") &&
           upsample.load(shaderRoot() / "gtao_upsample.comp.spv") &&
           m_gtaoPipeline.init(m_device, m_gtaoLayout, gtao.handle(), pipelineCache, sizeof(GtaoSettings)) &&
           m_denoisePipeline.init(m_device, m_denoiseLayout, denoise.handle(), pipelineCache,
                                  sizeof(DenoiseSettings)) &&
           m_upsamplePipeline.init(m_device, m_upsampleLayout, upsample.handle(), pipelineCache,
                                   sizeof(UpsampleSettings));
}

bool AmbientOcclusionPass::createImage(VkFormat format, VkImageUsageFlags usage, const char* name, Image& output) {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {m_storageExtent.width, m_storageExtent.height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = usage,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &output.image, &output.allocation, nullptr),
                     name)) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = output.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.view), name);
}

bool AmbientOcclusionPass::createImages(VkExtent2D storageExtent) {
    m_storageExtent = {std::max(storageExtent.width, 1u), std::max(storageExtent.height, 1u)};
    m_rawInitialized = false;
    if (m_computeSupported && !createImage(rawFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           "Create raw ambient-occlusion target", m_raw)) {
        return false;
    }
    const VkImageUsageFlags occlusionUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             (m_computeSupported ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    for (FrameOcclusion& frame : m_frames) {
        frame.contents = Contents::Undefined;
        if (!createImage(occlusionFormat, occlusionUsage, "Create ambient-occlusion target", frame)) {
            return false;
        }
    }
    return true;
}

bool AmbientOcclusionPass::resize(VkExtent2D storageExtent) {
    destroyImages();
    return createImages(storageExtent);
}

void AmbientOcclusionPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, AmbientOcclusionQuality quality,
                                  const AmbientOcclusionInput& input) {
    if (!m_computeSupported) {
        recordUnoccluded(commandBuffer, frameIndex);
        return;
    }
    const ambient_occlusion::GtaoQualityProfile profile = ambient_occlusion::qualityProfile(quality);
    const glm::uvec2 rawExtent = ambient_occlusion::dispatchExtent(toVector(input.renderExtent),
                                                                   profile.resolutionScale, toVector(m_storageExtent));
    recordGtao(commandBuffer, profile, rawExtent, input);
    if (profile.resolutionScale < 1.0f) {
        recordUpsample(commandBuffer, frameIndex, rawExtent, profile.parameters.occlusionPower, input);
    } else {
        recordDenoise(commandBuffer, frameIndex, rawExtent, profile.parameters.occlusionPower);
    }
    m_noiseFrame = input.temporalNoise ? m_noiseFrame + 1 : 0;
}

void AmbientOcclusionPass::recordGtao(VkCommandBuffer commandBuffer,
                                      const ambient_occlusion::GtaoQualityProfile& profile, glm::uvec2 dispatchExtent,
                                      const AmbientOcclusionInput& input) {
    const ambient_occlusion::GtaoParameters& parameters = profile.parameters;
    // A one-pixel render extent has no pyramid level 1.
    const uint32_t horizonLevel =
        std::min(parameters.horizonDepthLevel, depth_pyramid::levelCount(toVector(input.renderExtent)) - 1);
    // The previous frame's denoise read the raw target.
    imageBarrier(commandBuffer, m_raw.image, m_rawInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_rawInitialized = true;
    const VkDescriptorImageInfo pyramid{
        .sampler = input.depthPyramidSampler, .imageView = input.depthPyramid, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo normals{
        .sampler = m_pointSampler, .imageView = input.normals, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo raw{.imageView = m_raw.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 3> writes{{pushWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pyramid),
                                                      pushWrite(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normals),
                                                      pushWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &raw)}};
    const GtaoSettings settings{.inverseProjection = input.inverseProjection,
                                .dispatchExtent = glm::ivec2(dispatchExtent),
                                .renderExtent = glm::ivec2(toVector(input.renderExtent)),
                                .resolutionScale = profile.resolutionScale,
                                .radius = parameters.radius,
                                .falloffFraction = parameters.falloffFraction,
                                .projectionScale = input.projectionScale,
                                .noiseFrame = input.temporalNoise ? m_noiseFrame : 0u,
                                .sliceCount = parameters.sliceCount,
                                .stepsPerSide = parameters.stepsPerSide,
                                .horizonLevel = horizonLevel};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_gtaoPipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_gtaoPipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdPushConstants(commandBuffer, m_gtaoPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, groupCount(dispatchExtent.x), groupCount(dispatchExtent.y), 1);
    imageBarrier(commandBuffer, m_raw.image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void AmbientOcclusionPass::recordDenoise(VkCommandBuffer commandBuffer, uint32_t frameIndex, glm::uvec2 dispatchExtent,
                                         float occlusionPower) {
    FrameOcclusion& frame = m_frames[frameIndex];
    prepareOcclusionWrite(commandBuffer, frame);
    const VkDescriptorImageInfo raw{
        .sampler = m_pointSampler, .imageView = m_raw.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo occlusion{.imageView = frame.view, .imageLayout = occlusionLayout};
    const std::array<VkWriteDescriptorSet, 2> writes{{pushWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &raw),
                                                      pushWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &occlusion)}};
    const DenoiseSettings settings{.dispatchExtent = glm::ivec2(dispatchExtent), .occlusionPower = occlusionPower};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoisePipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoisePipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdPushConstants(commandBuffer, m_denoisePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, groupCount(dispatchExtent.x), groupCount(dispatchExtent.y), 1);
    finishOcclusionWrite(commandBuffer, frame);
}

void AmbientOcclusionPass::recordUpsample(VkCommandBuffer commandBuffer, uint32_t frameIndex, glm::uvec2 rawExtent,
                                          float occlusionPower, const AmbientOcclusionInput& input) {
    FrameOcclusion& frame = m_frames[frameIndex];
    prepareOcclusionWrite(commandBuffer, frame);
    const VkDescriptorImageInfo raw{
        .sampler = m_pointSampler, .imageView = m_raw.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo pyramid{
        .sampler = input.depthPyramidSampler, .imageView = input.depthPyramid, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo occlusion{.imageView = frame.view, .imageLayout = occlusionLayout};
    const std::array<VkWriteDescriptorSet, 3> writes{{pushWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &raw),
                                                      pushWrite(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pyramid),
                                                      pushWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &occlusion)}};
    // The output covers every render pixel plus one guard texel.
    const glm::uvec2 dispatchExtent =
        ambient_occlusion::dispatchExtent(toVector(input.renderExtent), 1.0f, toVector(m_storageExtent));
    // Positive view depth = P[3][2] / (depth + P[2][2]) for a reversed-Z
    // perspective projection, as in mesh.frag's clustered-lighting lookup.
    const glm::mat4 projection = glm::inverse(input.inverseProjection);
    const UpsampleSettings settings{.dispatchExtent = glm::ivec2(dispatchExtent),
                                    .renderExtent = glm::ivec2(toVector(input.renderExtent)),
                                    .rawExtent = glm::ivec2(rawExtent),
                                    .viewDepthTerms = {projection[3][2], projection[2][2]},
                                    .occlusionPower = occlusionPower};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsamplePipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsamplePipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdPushConstants(commandBuffer, m_upsamplePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, groupCount(dispatchExtent.x), groupCount(dispatchExtent.y), 1);
    finishOcclusionWrite(commandBuffer, frame);
}

void AmbientOcclusionPass::prepareOcclusionWrite(VkCommandBuffer commandBuffer, FrameOcclusion& frame) {
    // An earlier frame in this slot was read by the shading pass, or cleared.
    imageBarrier(
        commandBuffer, frame.image, frame.contents == Contents::Undefined ? VK_IMAGE_LAYOUT_UNDEFINED : occlusionLayout,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

void AmbientOcclusionPass::finishOcclusionWrite(VkCommandBuffer commandBuffer, FrameOcclusion& frame) {
    imageBarrier(commandBuffer, frame.image, occlusionLayout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    frame.contents = Contents::Occlusion;
}

void AmbientOcclusionPass::recordUnoccluded(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    FrameOcclusion& frame = m_frames[frameIndex];
    if (frame.contents == Contents::Unoccluded) {
        return;
    }
    imageBarrier(
        commandBuffer, frame.image, frame.contents == Contents::Undefined ? VK_IMAGE_LAYOUT_UNDEFINED : occlusionLayout,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue unoccluded{.float32 = {1.0f, 1.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = 1,
                                        .baseArrayLayer = 0,
                                        .layerCount = 1};
    vkCmdClearColorImage(commandBuffer, frame.image, occlusionLayout, &unoccluded, 1, &range);
    imageBarrier(commandBuffer, frame.image, occlusionLayout, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    frame.contents = Contents::Unoccluded;
}

uint64_t AmbientOcclusionPass::allocatedBytes() const {
    const uint64_t pixels = static_cast<uint64_t>(m_storageExtent.width) * m_storageExtent.height;
    // R16G16 raw target (when computed) plus one R8 target per frame slot.
    const uint64_t rawBytes = m_computeSupported ? pixels * sizeof(uint16_t) * 2 : 0u;
    return rawBytes + pixels * m_frames.size();
}

void AmbientOcclusionPass::destroyImage(Image& image) {
    if (image.view) {
        vkDestroyImageView(m_device, image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
    image = {};
}

void AmbientOcclusionPass::destroyImages() {
    destroyImage(m_raw);
    m_rawInitialized = false;
    for (FrameOcclusion& frame : m_frames) {
        destroyImage(frame);
        frame.contents = Contents::Undefined;
    }
}

void AmbientOcclusionPass::shutdown() {
    if (!m_device) {
        return;
    }
    destroyImages();
    m_frames.clear();
    m_gtaoPipeline.shutdown(m_device);
    m_denoisePipeline.shutdown(m_device);
    m_upsamplePipeline.shutdown(m_device);
    if (m_pointSampler) {
        vkDestroySampler(m_device, m_pointSampler, nullptr);
    }
    if (m_linearSampler) {
        vkDestroySampler(m_device, m_linearSampler, nullptr);
    }
    if (m_gtaoLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_gtaoLayout, nullptr);
    }
    if (m_denoiseLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_denoiseLayout, nullptr);
    }
    if (m_upsampleLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_upsampleLayout, nullptr);
    }
    m_pointSampler = VK_NULL_HANDLE;
    m_linearSampler = VK_NULL_HANDLE;
    m_gtaoLayout = VK_NULL_HANDLE;
    m_denoiseLayout = VK_NULL_HANDLE;
    m_upsampleLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_noiseFrame = 0;
    m_computeSupported = false;
}
