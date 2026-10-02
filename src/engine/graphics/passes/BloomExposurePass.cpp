#include "BloomExposurePass.hpp"

#include "../ToneMapping.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace {
constexpr uint32_t histogramGroupSize = 16;
constexpr uint32_t bloomGroupSize = 8;
constexpr uint32_t exposureFlagAutomatic = 1u;
constexpr uint32_t exposureFlagReset = 2u;

// Push-constant mirrors of the compute shaders; keep the layouts in sync.
struct HistogramSettings {
    glm::uvec2 validExtent{0u};
    float minimumLogLuminance{0.0f};
    float inverseLogLuminanceRange{0.0f};
};

struct AdaptSettings {
    float minimumLogLuminance{0.0f};
    float maximumLogLuminance{0.0f};
    float lowPercentile{0.0f};
    float highPercentile{0.0f};
    float compensationEv{0.0f};
    float manualEv{0.0f};
    float adaptationSpeedUp{0.0f};
    float adaptationSpeedDown{0.0f};
    float deltaSeconds{0.0f};
    uint32_t flags{0u};
};

struct DownsampleSettings {
    glm::vec2 sourceTexelSize{0.0f};
    glm::vec2 sourceUvLimit{0.0f};
    glm::uvec2 destinationExtent{0u};
    uint32_t karisAverage{0u};
};

struct UpsampleSettings {
    glm::vec2 sourceTexelSize{0.0f};
    glm::vec2 sourceUvLimit{0.0f};
    glm::uvec2 destinationExtent{0u};
    float filterRadius{1.0f};
    float outputScale{1.0f};
};

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type) {
    return {
        .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

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

void computeToComputeBarrier(VkCommandBuffer commandBuffer) {
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// Later frames' consumers may be fragment (post) or compute (resolve) shaders.
constexpr VkPipelineStageFlags2 consumerStages =
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

uint32_t groupsFor(uint32_t count, uint32_t groupSize) {
    return (count + groupSize - 1) / groupSize;
}

glm::vec2 texelSize(VkExtent2D extent) {
    return {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
}

// Largest UV whose bilinear footprint stays inside the valid region.
glm::vec2 uvLimit(VkExtent2D validExtent, VkExtent2D allocatedExtent) {
    return (glm::vec2(validExtent.width, validExtent.height) - 0.5f) * texelSize(allocatedExtent);
}
} // namespace

bool BloomExposurePass::init(const VulkanContext& context) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create bloom sampler") ||
        !createLayouts() || !createPipelines(context.pipelineCache()) ||
        !createBuffer(sizeof(uint32_t) * luminanceHistogramBinCount,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_histogram) ||
        !createBuffer(exposureBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_exposure)) {
        shutdown(context);
        return false;
    }
    m_exposureValid = false;
    return true;
}

bool BloomExposurePass::createLayouts() {
    const auto create = [&](std::span<const VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout& layout,
                            const char* operation) {
        // Push descriptors: the HDR input view changes per frame slot, so the
        // bindings are recorded inline instead of in per-frame descriptor sets.
        const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                   .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                   .pBindings = bindings.data()};
        return checkVulkan(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout), operation);
    };
    const std::array histogramBindings{computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                       computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    const std::array adaptBindings{computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                   computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    const std::array bloomBindings{computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                   computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    return create(histogramBindings, m_histogramLayout, "Create luminance-histogram descriptor layout") &&
           create(adaptBindings, m_adaptLayout, "Create exposure-adaptation descriptor layout") &&
           create(bloomBindings, m_bloomLayout, "Create bloom descriptor layout");
}

bool BloomExposurePass::createPipelines(VkPipelineCache pipelineCache) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule histogram(m_device);
    ShaderModule adapt(m_device);
    ShaderModule downsample(m_device);
    ShaderModule upsample(m_device);
    if (!histogram.load(shaders / "luminance_histogram.comp.spv") || !adapt.load(shaders / "exposure_adapt.comp.spv") ||
        !downsample.load(shaders / "bloom_downsample.comp.spv") ||
        !upsample.load(shaders / "bloom_upsample.comp.spv")) {
        return false;
    }
    return m_histogramPipeline.init(m_device, m_histogramLayout, histogram.handle(), pipelineCache,
                                    sizeof(HistogramSettings)) &&
           m_adaptPipeline.init(m_device, m_adaptLayout, adapt.handle(), pipelineCache, sizeof(AdaptSettings)) &&
           m_downsamplePipeline.init(m_device, m_bloomLayout, downsample.handle(), pipelineCache,
                                     sizeof(DownsampleSettings)) &&
           m_upsamplePipeline.init(m_device, m_bloomLayout, upsample.handle(), pipelineCache, sizeof(UpsampleSettings));
}

bool BloomExposurePass::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferAllocation& output) {
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, nullptr),
                       "Create bloom/exposure buffer");
}

bool BloomExposurePass::createChain(const VulkanContext& context, VkExtent2D maximumExtent) {
    destroyChain(context);
    m_chainExtent = {std::max(1u, maximumExtent.width / 2), std::max(1u, maximumExtent.height / 2)};
    const uint32_t shortestSide = std::min(m_chainExtent.width, m_chainExtent.height);
    m_mipCount = std::min(maximumBloomMips, 1u + static_cast<uint32_t>(std::floor(std::log2(shortestSide))));
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = bloomFormat,
                                  .extent = {.width = m_chainExtent.width, .height = m_chainExtent.height, .depth = 1},
                                  .mipLevels = m_mipCount,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_bloomImage, &m_bloomAllocation, nullptr),
                     "Create bloom chain")) {
        return false;
    }
    for (uint32_t mip = 0; mip < m_mipCount; ++mip) {
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = m_bloomImage,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = bloomFormat,
                                         .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .baseMipLevel = mip,
                                                              .levelCount = 1,
                                                              .baseArrayLayer = 0,
                                                              .layerCount = 1}};
        if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_mipViews[mip]), "Create bloom mip view")) {
            return false;
        }
    }
    m_chainInitialized = false;
    return true;
}

void BloomExposurePass::destroyChain(const VulkanContext& context) {
    for (VkImageView& view : m_mipViews) {
        if (view) {
            vkDestroyImageView(context.device(), view, nullptr);
        }
        view = VK_NULL_HANDLE;
    }
    if (m_bloomImage) {
        vmaDestroyImage(context.vmaAllocator(), m_bloomImage, m_bloomAllocation);
    }
    m_bloomImage = VK_NULL_HANDLE;
    m_bloomAllocation = VK_NULL_HANDLE;
    m_chainExtent = {};
    m_mipCount = 0;
    m_chainInitialized = false;
}

void BloomExposurePass::shutdown(const VulkanContext& context) {
    destroyChain(context);
    const VkDevice device = context.device();
    m_histogramPipeline.shutdown(device);
    m_adaptPipeline.shutdown(device);
    m_downsamplePipeline.shutdown(device);
    m_upsamplePipeline.shutdown(device);
    for (BufferAllocation* buffer : {&m_histogram, &m_exposure}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), buffer->buffer, buffer->allocation);
        }
        *buffer = {};
    }
    for (VkDescriptorSetLayout* layout : {&m_histogramLayout, &m_adaptLayout, &m_bloomLayout}) {
        if (*layout) {
            vkDestroyDescriptorSetLayout(device, *layout, nullptr);
        }
        *layout = VK_NULL_HANDLE;
    }
    if (m_sampler) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_exposureValid = false;
}

void BloomExposurePass::recordExposure(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                                       const ExposureSettings& settings, float deltaSeconds) {
    const bool automatic = settings.mode == ExposureMode::Auto;
    // Orders this frame's writes after earlier frames' histogram and exposure readers.
    memoryBarrier(commandBuffer, consumerStages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (automatic) {
        vkCmdFillBuffer(commandBuffer, m_histogram.buffer, 0, VK_WHOLE_SIZE, 0);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const VkDescriptorImageInfo image{.sampler = m_sampler, .imageView = input.view, .imageLayout = input.layout};
        const VkDescriptorBufferInfo histogram{.buffer = m_histogram.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstBinding = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &image},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstBinding = 1,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .pBufferInfo = &histogram}};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_histogramPipeline.handle());
        vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_histogramPipeline.layout(), 0,
                               static_cast<uint32_t>(std::size(writes)), writes);
        const HistogramSettings histogramSettings{
            .validExtent = {input.validExtent.width, input.validExtent.height},
            .minimumLogLuminance = settings.minimumLogLuminance,
            .inverseLogLuminanceRange = 1.0f / (settings.maximumLogLuminance - settings.minimumLogLuminance)};
        vkCmdPushConstants(commandBuffer, m_histogramPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(histogramSettings), &histogramSettings);
        vkCmdDispatch(commandBuffer, groupsFor(input.validExtent.width, histogramGroupSize),
                      groupsFor(input.validExtent.height, histogramGroupSize), 1);
        computeToComputeBarrier(commandBuffer);
    }

    const VkDescriptorBufferInfo histogram{.buffer = m_histogram.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo exposure{.buffer = m_exposure.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &histogram},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstBinding = 1,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &exposure}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_adaptPipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_adaptPipeline.layout(), 0,
                           static_cast<uint32_t>(std::size(writes)), writes);
    const AdaptSettings adaptSettings{.minimumLogLuminance = settings.minimumLogLuminance,
                                      .maximumLogLuminance = settings.maximumLogLuminance,
                                      .lowPercentile = settings.lowPercentile,
                                      .highPercentile = settings.highPercentile,
                                      .compensationEv = settings.compensationEv,
                                      .manualEv = settings.manualEv,
                                      .adaptationSpeedUp = settings.adaptationSpeedUp,
                                      .adaptationSpeedDown = settings.adaptationSpeedDown,
                                      .deltaSeconds = deltaSeconds,
                                      .flags = (automatic ? exposureFlagAutomatic : 0u) |
                                               (m_exposureValid ? 0u : exposureFlagReset)};
    vkCmdPushConstants(commandBuffer, m_adaptPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(adaptSettings),
                       &adaptSettings);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    m_exposureValid = true;
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  consumerStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void BloomExposurePass::initializeBloomLayout(VkCommandBuffer commandBuffer) {
    if (m_chainInitialized || m_mipCount == 0) {
        return;
    }
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                        .srcAccessMask = 0,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        .dstAccessMask =
                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = m_bloomImage,
                                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                             .baseMipLevel = 0,
                                                             .levelCount = m_mipCount,
                                                             .baseArrayLayer = 0,
                                                             .layerCount = 1}};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    m_chainInitialized = true;
}

void BloomExposurePass::pushSampledAndStorage(VkCommandBuffer commandBuffer, const ComputePipeline& pipeline,
                                              VkImageView sampled, VkImageLayout sampledLayout, VkImageView storage) {
    const VkDescriptorImageInfo source{.sampler = m_sampler, .imageView = sampled, .imageLayout = sampledLayout};
    const VkDescriptorImageInfo destination{.imageView = storage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &source},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstBinding = 1,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         .pImageInfo = &destination}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0,
                           static_cast<uint32_t>(std::size(writes)), writes);
}

void BloomExposurePass::recordBloom(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                                    float filterRadius) {
    if (m_mipCount == 0) {
        return;
    }
    if (m_chainInitialized) {
        // The chain stays in GENERAL; only order against earlier frames' readers.
        memoryBarrier(commandBuffer, consumerStages, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    } else {
        initializeBloomLayout(commandBuffer);
    }
    for (uint32_t mip = 0; mip < m_mipCount; ++mip) {
        const bool fromScene = mip == 0;
        const VkExtent2D sourceAllocated = fromScene ? input.imageExtent : allocatedMipExtent(mip - 1);
        const VkExtent2D sourceValid = fromScene ? input.validExtent : validMipExtent(input.validExtent, mip - 1);
        const VkExtent2D destinationValid = validMipExtent(input.validExtent, mip);
        pushSampledAndStorage(commandBuffer, m_downsamplePipeline, fromScene ? input.view : m_mipViews[mip - 1],
                              fromScene ? input.layout : VK_IMAGE_LAYOUT_GENERAL, m_mipViews[mip]);
        const DownsampleSettings downsample{.sourceTexelSize = texelSize(sourceAllocated),
                                            .sourceUvLimit = uvLimit(sourceValid, sourceAllocated),
                                            .destinationExtent = {destinationValid.width, destinationValid.height},
                                            .karisAverage = fromScene ? 1u : 0u};
        vkCmdPushConstants(commandBuffer, m_downsamplePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(downsample), &downsample);
        vkCmdDispatch(commandBuffer, groupsFor(destinationValid.width, bloomGroupSize),
                      groupsFor(destinationValid.height, bloomGroupSize), 1);
        computeToComputeBarrier(commandBuffer);
    }
    // Each upsample adds the blurred smaller mip into the next larger one, so mip
    // 0 ends up holding the sum of every level; the last step averages it.
    for (uint32_t mip = m_mipCount - 1; mip > 0; --mip) {
        const VkExtent2D sourceAllocated = allocatedMipExtent(mip);
        const VkExtent2D sourceValid = validMipExtent(input.validExtent, mip);
        const VkExtent2D destinationValid = validMipExtent(input.validExtent, mip - 1);
        pushSampledAndStorage(commandBuffer, m_upsamplePipeline, m_mipViews[mip], VK_IMAGE_LAYOUT_GENERAL,
                              m_mipViews[mip - 1]);
        const UpsampleSettings upsample{.sourceTexelSize = texelSize(sourceAllocated),
                                        .sourceUvLimit = uvLimit(sourceValid, sourceAllocated),
                                        .destinationExtent = {destinationValid.width, destinationValid.height},
                                        .filterRadius = filterRadius,
                                        .outputScale = mip == 1 ? 1.0f / static_cast<float>(m_mipCount) : 1.0f};
        vkCmdPushConstants(commandBuffer, m_upsamplePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(upsample),
                           &upsample);
        vkCmdDispatch(commandBuffer, groupsFor(destinationValid.width, bloomGroupSize),
                      groupsFor(destinationValid.height, bloomGroupSize), 1);
        computeToComputeBarrier(commandBuffer);
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  consumerStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

BloomSampling BloomExposurePass::bloomSampling(VkExtent2D validExtent) const {
    if (m_mipCount == 0) {
        return {};
    }
    const VkExtent2D allocated = allocatedMipExtent(0);
    const glm::vec2 texel = texelSize(allocated);
    return {.uvScale = glm::vec2(validExtent.width, validExtent.height) * 0.5f * texel,
            .uvLimit = uvLimit(validMipExtent(validExtent, 0), allocated)};
}

VkExtent2D BloomExposurePass::allocatedMipExtent(uint32_t mip) const {
    return {std::max(1u, m_chainExtent.width >> mip), std::max(1u, m_chainExtent.height >> mip)};
}

VkExtent2D BloomExposurePass::validMipExtent(VkExtent2D validExtent, uint32_t mip) const {
    const VkExtent2D allocated = allocatedMipExtent(mip);
    return {std::clamp(validExtent.width >> (mip + 1), 1u, allocated.width),
            std::clamp(validExtent.height >> (mip + 1), 1u, allocated.height)};
}
