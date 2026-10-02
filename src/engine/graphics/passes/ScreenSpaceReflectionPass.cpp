#include "ScreenSpaceReflectionPass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>

namespace {
constexpr uint32_t workgroupSize = 8;
constexpr uint32_t flagHalfResolution = 1u;
constexpr uint32_t flagTemporalJitter = 2u;
// View-space reach of a reflection ray before clipping to the screen.
constexpr float maximumRayDistance = 60.0f;
// Width of the screen-border fade as a fraction of each axis.
constexpr float edgeFadeFraction = 0.08f;
// Origin lift along the normal, per unit of view distance.
constexpr float originNormalOffset = 0.002f;

// Mirrors TraceConstants in shaders/ssr_trace.comp.
struct TraceConstants {
    glm::vec4 projection{0.0f};
    glm::vec4 depthTerms{0.0f};
    glm::vec4 tuning{0.0f};
    glm::uvec4 extents{0u};
    glm::uvec4 parameters{0u};
    glm::vec2 inverseStorageExtent{0.0f};
    glm::uvec2 footprintOffset{0u};
};

// Mirrors CompositeConstants in shaders/ssr_composite.comp.
struct CompositeConstants {
    glm::vec4 projection{0.0f};
    glm::vec4 depthTerms{0.0f};
    glm::vec4 tuning{0.0f};
    glm::uvec4 extents{0u};
    glm::vec4 footprint{0.0f};
    glm::vec2 inverseTraceStorageExtent{0.0f};
};

glm::vec4 packedProjection(const screen_space_reflection::PerspectiveTerms& terms) {
    return {terms.scale, terms.offset};
}

glm::vec2 inverseExtent(VkExtent2D extent) {
    return {1.0f / static_cast<float>(std::max(extent.width, 1u)),
            1.0f / static_cast<float>(std::max(extent.height, 1u))};
}

// Render pixel that traced texel 0 of each 2x2 block samples at half
// resolution. It rotates over frames when the temporal resolve accumulates,
// so every pixel of the block is traced within four frames.
glm::uvec2 footprintOffset(ReflectionQuality quality, const ScreenSpaceReflectionCamera& camera) {
    if (quality != ReflectionQuality::ScreenSpaceHalfResolution || !camera.temporalAccumulation) {
        return glm::uvec2(0u);
    }
    return {camera.frameNumber & 1u, (camera.frameNumber >> 1u) & 1u};
}

uint32_t groupsFor(uint32_t count) {
    return (count + workgroupSize - 1) / workgroupSize;
}

VkWriteDescriptorSet imageWrite(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = image};
}

void targetBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                   VkPipelineStageFlags2 sourceStages, VkAccessFlags2 sourceAccess, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStages,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
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
} // namespace

bool ScreenSpaceReflectionPass::init(const VulkanContext& context, VkExtent2D outputExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = outputExtent;
    ShaderModule trace(m_device);
    ShaderModule composite(m_device);
    if (!createLayout() || !trace.load(shaderRoot() / "ssr_trace.comp.spv") ||
        !composite.load(shaderRoot() / "ssr_composite.comp.spv") ||
        !m_tracePipeline.init(m_device, m_layout, trace.handle(), context.pipelineCache(), sizeof(TraceConstants)) ||
        !m_compositePipeline.init(m_device, m_layout, composite.handle(), context.pipelineCache(),
                                  sizeof(CompositeConstants))) {
        shutdown();
        return false;
    }
    return true;
}

bool ScreenSpaceReflectionPass::createLayout() {
    // Both shaders share one push-descriptor layout: sampled inputs at 0-2,
    // one storage image at 3, and the prepass specular target at 4, which only
    // the composite reads. The inputs change per frame slot.
    constexpr uint32_t storageBinding = 3;
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {.binding = binding,
                             .descriptorType = binding == storageBinding ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .descriptorCount = 1,
                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create screen-space reflection descriptor layout")) {
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
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler),
                       "Create screen-space reflection sampler");
}

bool ScreenSpaceReflectionPass::ensureTarget() {
    if (m_target) {
        return true;
    }
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = targetFormat,
                                  .extent = {.width = m_extent.width, .height = m_extent.height, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_target, &m_targetAllocation, nullptr),
                     "Create screen-space reflection target")) {
        destroyTarget();
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = m_target,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = targetFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_targetView),
                     "Create screen-space reflection target view")) {
        destroyTarget();
        return false;
    }
    return true;
}

void ScreenSpaceReflectionPass::destroyTarget() {
    if (m_targetView) {
        vkDestroyImageView(m_device, m_targetView, nullptr);
    }
    if (m_target) {
        vmaDestroyImage(m_allocator, m_target, m_targetAllocation);
    }
    m_targetView = VK_NULL_HANDLE;
    m_target = VK_NULL_HANDLE;
    m_targetAllocation = VK_NULL_HANDLE;
}

void ScreenSpaceReflectionPass::resize(VkExtent2D outputExtent) {
    destroyTarget();
    m_extent = outputExtent;
}

void ScreenSpaceReflectionPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroyTarget();
    m_tracePipeline.shutdown(m_device);
    m_compositePipeline.shutdown(m_device);
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}

VkExtent2D ScreenSpaceReflectionPass::tracedExtent(VkExtent2D renderExtent, ReflectionQuality quality) const {
    if (quality == ReflectionQuality::ScreenSpaceHalfResolution) {
        return {std::max((renderExtent.width + 1) / 2, 1u), std::max((renderExtent.height + 1) / 2, 1u)};
    }
    return renderExtent;
}

bool ScreenSpaceReflectionPass::record(VkCommandBuffer commandBuffer, VkImageView sceneView,
                                       const SceneGeometryViews& geometry, ReflectionQuality quality,
                                       const ScreenSpaceReflectionCamera& camera) {
    if (!ensureTarget()) {
        return false;
    }
    const VkExtent2D traced = tracedExtent(geometry.renderExtent, quality);
    // Every traced texel is rewritten; earlier frames' composites read it.
    targetBarrier(commandBuffer, m_target, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    recordTrace(commandBuffer, sceneView, geometry, traced, quality, camera);
    // The composite samples the trace and rewrites the scene the trace sampled.
    const VkMemoryBarrier2 traceToComposite{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &traceToComposite};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    recordComposite(commandBuffer, sceneView, geometry, traced, quality, camera);
    return true;
}

void ScreenSpaceReflectionPass::recordTrace(VkCommandBuffer commandBuffer, VkImageView sceneView,
                                            const SceneGeometryViews& geometry, VkExtent2D traced,
                                            ReflectionQuality quality,
                                            const ScreenSpaceReflectionCamera& camera) const {
    const VkDescriptorImageInfo images[]{
        {.sampler = m_sampler, .imageView = sceneView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = m_sampler, .imageView = geometry.normals, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = geometry.depthPyramidSampler,
         .imageView = geometry.depthPyramid,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = m_targetView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL}};
    const VkWriteDescriptorSet writes[]{imageWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0]),
                                        imageWrite(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1]),
                                        imageWrite(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[2]),
                                        imageWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[3])};
    const screen_space_reflection::PerspectiveTerms terms =
        screen_space_reflection::perspectiveTerms(camera.projection);
    const uint32_t flags = (quality == ReflectionQuality::ScreenSpaceHalfResolution ? flagHalfResolution : 0u) |
                           (camera.temporalAccumulation ? flagTemporalJitter : 0u);
    const TraceConstants constants{
        .projection = packedProjection(terms),
        .depthTerms = {terms.depthScale, terms.depthBias, maximumRayDistance, m_traceSettings.thickness},
        .tuning = {m_traceSettings.thicknessPerDistance, edgeFadeFraction, m_compositeSettings.roughnessFadeEnd,
                   originNormalOffset},
        .extents = {geometry.renderExtent.width, geometry.renderExtent.height, traced.width, traced.height},
        .parameters = {geometry.depthPyramidLevels, m_traceSettings.maximumIterations, camera.frameNumber, flags},
        .inverseStorageExtent = inverseExtent(geometry.storageExtent),
        .footprintOffset = footprintOffset(quality, camera)};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline.layout(), 0,
                           static_cast<uint32_t>(std::size(writes)), writes);
    vkCmdPushConstants(commandBuffer, m_tracePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(commandBuffer, groupsFor(traced.width), groupsFor(traced.height), 1);
}

void ScreenSpaceReflectionPass::recordComposite(VkCommandBuffer commandBuffer, VkImageView sceneView,
                                                const SceneGeometryViews& geometry, VkExtent2D traced,
                                                ReflectionQuality quality,
                                                const ScreenSpaceReflectionCamera& camera) const {
    const VkDescriptorImageInfo images[]{
        {.sampler = m_sampler, .imageView = m_targetView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = m_sampler, .imageView = geometry.normals, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = geometry.depthPyramidSampler,
         .imageView = geometry.depthPyramid,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = sceneView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = m_sampler,
         .imageView = geometry.specular,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkWriteDescriptorSet writes[]{imageWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0]),
                                        imageWrite(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1]),
                                        imageWrite(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[2]),
                                        imageWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[3]),
                                        imageWrite(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[4])};
    const screen_space_reflection::PerspectiveTerms terms =
        screen_space_reflection::perspectiveTerms(camera.projection);
    const CompositeConstants constants{
        .projection = packedProjection(terms),
        .depthTerms = {terms.depthScale, terms.depthBias, 0.0f, 0.0f},
        .tuning = {m_compositeSettings.roughnessFadeStart, m_compositeSettings.roughnessFadeEnd,
                   m_compositeSettings.intensity, 0.0f},
        .extents = {geometry.renderExtent.width, geometry.renderExtent.height, traced.width, traced.height},
        .footprint = {glm::vec2(footprintOffset(quality, camera)),
                      quality == ReflectionQuality::ScreenSpaceHalfResolution ? 0.5f : 1.0f, 0.0f},
        .inverseTraceStorageExtent = inverseExtent(m_extent)};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeline.layout(), 0,
                           static_cast<uint32_t>(std::size(writes)), writes);
    vkCmdPushConstants(commandBuffer, m_compositePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(commandBuffer, groupsFor(geometry.renderExtent.width), groupsFor(geometry.renderExtent.height), 1);
}

uint64_t ScreenSpaceReflectionPass::targetBytes() const {
    if (!m_target) {
        return 0;
    }
    return static_cast<uint64_t>(m_extent.width) * m_extent.height * sizeof(uint16_t) * 4;
}
