#include "SpatialUpscalePass.hpp"

#include "engine/graphics/SpatialUpscale.hpp"
#include "engine/graphics/resources/FrameTimestamps.hpp"
#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <SDL3/SDL_stdinc.h>

#include <array>

namespace {
constexpr uint32_t workgroupSize = 8;

// Mirrors EasuSettings in shaders/fsr_easu.comp.
struct EasuSettings {
    glm::vec2 scale{1.0f};
    glm::vec2 offset{0.0f};
    glm::ivec2 inputMaximum{0};
    glm::ivec2 outputSize{1};
};

// Mirrors RcasSettings in shaders/fsr_rcas.frag.
struct RcasSettings {
    glm::ivec2 outputMaximum{0};
    float lobeScale{1.0f};
    float padding{0.0f};
};

bool hasFeatures(const VulkanContext& context, VkFormat format, VkFormatFeatureFlags required) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context.pDevice(), format, &properties);
    return (properties.optimalTilingFeatures & required) == required;
}

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStage,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStage,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = newLayout,
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

void writeTimestamp(VkCommandBuffer commandBuffer, VkQueryPool queries, VkPipelineStageFlags2 stage, uint32_t query) {
    if (queries) {
        vkCmdWriteTimestamp2(commandBuffer, stage, queries, query);
    }
}
} // namespace

bool SpatialUpscalePass::init(const VulkanContext& context, VkFormat outputFormat, VkFormat gradeFormat,
                              VkExtent2D outputExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_outputFormat = outputFormat;
    m_extent = outputExtent;
    m_supported = hasFeatures(context, upscaledFormat,
                              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                  hasFeatures(context, outputFormat,
                              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (!m_supported) {
        context.log().info("FSR 1 upscaling disabled: the output or RGBA8 storage format lacks required features");
        return true;
    }
    // ZENITH_DISABLE_SPATIAL_UPSCALING reports FSR 1 as unsupported, so the
    // capability fallback can be validated on devices that have it.
    if (SDL_getenv("ZENITH_DISABLE_SPATIAL_UPSCALING") != nullptr) {
        m_supported = false;
        context.log().info("FSR 1 upscaling disabled by ZENITH_DISABLE_SPATIAL_UPSCALING");
        return true;
    }
    ShaderModule easu(m_device);
    if (!createDescriptors() || !easu.load(shaderRoot() / "fsr_easu.comp.spv") ||
        !m_easuPipeline.init(m_device, m_easuLayout, easu.handle(), context.pipelineCache(), sizeof(EasuSettings)) ||
        !createRcasPipelineLayout() || !createRcasPipeline(outputFormat, context.pipelineCache(), m_rcasPipeline) ||
        !createRcasPipeline(gradeFormat, context.pipelineCache(), m_rcasGradePipeline)) {
        shutdown();
        return false;
    }
    return true;
}

bool SpatialUpscalePass::createDescriptors() {
    const std::array<VkDescriptorSetLayoutBinding, 2> easuBindings{
        {{.binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
         {.binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}}};
    const VkDescriptorSetLayoutBinding rcasBinding{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkDescriptorSetLayoutCreateInfo easuLayout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                     .bindingCount = static_cast<uint32_t>(easuBindings.size()),
                                                     .pBindings = easuBindings.data()};
    const VkDescriptorSetLayoutCreateInfo rcasLayout{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &rcasBinding};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &easuLayout, nullptr, &m_easuLayout),
                     "Create FSR EASU descriptor layout") ||
        !checkVulkan(vkCreateDescriptorSetLayout(m_device, &rcasLayout, nullptr, &m_rcasLayout),
                     "Create FSR RCAS descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize sizes[]{{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2},
                                       {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = 2,
                                          .poolSizeCount = static_cast<uint32_t>(std::size(sizes)),
                                          .pPoolSizes = sizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool), "Create FSR descriptor pool")) {
        return false;
    }
    const VkDescriptorSetLayout layouts[]{m_easuLayout, m_rcasLayout};
    VkDescriptorSet descriptors[2]{};
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = 2,
                                               .pSetLayouts = layouts};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, descriptors), "Allocate FSR descriptors")) {
        return false;
    }
    m_easuDescriptor = descriptors[0];
    m_rcasDescriptor = descriptors[1];
    // Both passes fetch exact texels; nearest filtering keeps the sampler inert.
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create FSR sampler");
}

bool SpatialUpscalePass::createRcasPipelineLayout() {
    const VkPushConstantRange push{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(RcasSettings)};
    const VkPipelineLayoutCreateInfo pipelineLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .setLayoutCount = 1,
                                                    .pSetLayouts = &m_rcasLayout,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push};
    if (!checkVulkan(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_rcasPipelineLayout),
                     "Create FSR RCAS pipeline layout")) {
        return false;
    }
    return true;
}

bool SpatialUpscalePass::createRcasPipeline(VkFormat targetFormat, VkPipelineCache pipelineCache, VkPipeline& output) {
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaderRoot() / "fullscreen.vert.spv") || !fragment.load(shaderRoot() / "fsr_rcas.frag.spv")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                    .module = vertex.handle(),
                                                    .pName = "main"},
                                                   {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                    .module = fragment.handle(),
                                                    .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster{.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                        .polygonMode = VK_POLYGON_MODE_FILL,
                                                        .cullMode = VK_CULL_MODE_NONE,
                                                        .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample{.sType =
                                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState colorAttachment{.colorWriteMask =
                                                                  VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &colorAttachment};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = static_cast<uint32_t>(std::size(states)),
                                                   .pDynamicStates = states};
    const VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                  .colorAttachmentCount = 1,
                                                  .pColorAttachmentFormats = &targetFormat};
    const VkGraphicsPipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = &rendering,
                                                .stageCount = static_cast<uint32_t>(std::size(stages)),
                                                .pStages = stages,
                                                .pVertexInputState = &vertexInput,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewport,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &multisample,
                                                .pColorBlendState = &blend,
                                                .pDynamicState = &dynamic,
                                                .layout = m_rcasPipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(m_device, pipelineCache, 1, &pipeline, nullptr, &output),
                       "Create FSR RCAS pipeline");
}

bool SpatialUpscalePass::createTarget(Target& target, VkFormat format, VkImageUsageFlags usage) {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {.width = m_extent.width, .height = m_extent.height, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = usage,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &target.image, &target.allocation, nullptr),
                     "Create FSR target")) {
        target = {};
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = target.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &target.view), "Create FSR target view");
}

bool SpatialUpscalePass::ensureTargets() {
    if (m_input.image) {
        return true;
    }
    if (!createTarget(m_input, m_outputFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !createTarget(m_upscaled, upscaledFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        destroyTargets();
        return false;
    }
    writeDescriptors();
    m_targetsUsed = false;
    return true;
}

void SpatialUpscalePass::writeDescriptors() {
    const VkDescriptorImageInfo input{
        .sampler = m_sampler, .imageView = m_input.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo storage{.imageView = m_upscaled.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo upscaled{
        .sampler = m_sampler, .imageView = m_upscaled.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_easuDescriptor,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &input},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_easuDescriptor,
                                         .dstBinding = 1,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         .pImageInfo = &storage},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_rcasDescriptor,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &upscaled}};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

void SpatialUpscalePass::destroyTargets() {
    for (Target* target : {&m_input, &m_upscaled}) {
        if (target->view) {
            vkDestroyImageView(m_device, target->view, nullptr);
        }
        if (target->image) {
            vmaDestroyImage(m_allocator, target->image, target->allocation);
        }
        *target = {};
    }
    m_targetsUsed = false;
}

void SpatialUpscalePass::resize(VkExtent2D outputExtent) {
    destroyTargets();
    m_extent = outputExtent;
}

VkImageView SpatialUpscalePass::prepareInput(VkCommandBuffer commandBuffer) {
    if (!m_supported || !ensureTargets()) {
        return VK_NULL_HANDLE;
    }
    // The previous frame's EASU sampled the input; this frame overwrites it.
    imageBarrier(commandBuffer, m_input.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 m_targetsUsed ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return m_input.view;
}

void SpatialUpscalePass::recordEasu(VkCommandBuffer commandBuffer, VkExtent2D inputExtent,
                                    VkQueryPool timestampQueries) {
    writeTimestamp(commandBuffer, timestampQueries, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, FrameTimestamp::fsrEasuBegin);
    imageBarrier(commandBuffer, m_input.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The previous frame's RCAS sampled the upscaled image; every texel is
    // overwritten below.
    imageBarrier(commandBuffer, m_upscaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 m_targetsUsed ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const EasuConstants constants =
        easuConstants({inputExtent.width, inputExtent.height}, {m_extent.width, m_extent.height});
    const EasuSettings settings{.scale = constants.scale,
                                .offset = constants.offset,
                                .inputMaximum = constants.inputMaximum,
                                .outputSize = constants.outputSize};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipeline.layout(), 0, 1,
                            &m_easuDescriptor, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_easuPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, (m_extent.width + workgroupSize - 1) / workgroupSize,
                  (m_extent.height + workgroupSize - 1) / workgroupSize, 1);
    imageBarrier(commandBuffer, m_upscaled.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_targetsUsed = true;
    writeTimestamp(commandBuffer, timestampQueries, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, FrameTimestamp::fsrEasuEnd);
}

void SpatialUpscalePass::recordRcas(VkCommandBuffer commandBuffer, VkImageView targetView, VkFormat targetFormat,
                                    float sharpness, VkQueryPool timestampQueries) {
    writeTimestamp(commandBuffer, timestampQueries, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, FrameTimestamp::fsrRcasBegin);
    const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                          .imageView = targetView,
                                          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = m_extent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{.x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(m_extent.width),
                              .height = static_cast<float>(m_extent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    const VkRect2D scissor{.offset = {0, 0}, .extent = m_extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    const RcasSettings settings{.outputMaximum = glm::ivec2(m_extent.width, m_extent.height) - 1,
                                .lobeScale = rcasLobeScale(sharpness)};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      targetFormat == m_outputFormat ? m_rcasPipeline : m_rcasGradePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rcasPipelineLayout, 0, 1,
                            &m_rcasDescriptor, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_rcasPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
    writeTimestamp(commandBuffer, timestampQueries, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, FrameTimestamp::fsrRcasEnd);
}

void SpatialUpscalePass::writeSkippedTimestamps(VkCommandBuffer commandBuffer, VkQueryPool timestampQueries) {
    for (uint32_t query = FrameTimestamp::fsrEasuBegin; query <= FrameTimestamp::fsrRcasEnd; ++query) {
        writeTimestamp(commandBuffer, timestampQueries, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, query);
    }
}

uint64_t SpatialUpscalePass::targetBytes() const {
    if (!m_input.image) {
        return 0;
    }
    // The upscaled target is RGBA8; the input matches the output format,
    // which is 8 bits per channel unless it is a half-float HDR format.
    const uint64_t inputBytesPerTexel = m_outputFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    return static_cast<uint64_t>(m_extent.width) * m_extent.height * (inputBytesPerTexel + 4);
}

void SpatialUpscalePass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroyTargets();
    m_easuPipeline.shutdown(m_device);
    for (VkPipeline pipeline : {m_rcasPipeline, m_rcasGradePipeline}) {
        if (pipeline) {
            vkDestroyPipeline(m_device, pipeline, nullptr);
        }
    }
    if (m_rcasPipelineLayout) {
        vkDestroyPipelineLayout(m_device, m_rcasPipelineLayout, nullptr);
    }
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_easuLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_easuLayout, nullptr);
    }
    if (m_rcasLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_rcasLayout, nullptr);
    }
    m_rcasPipeline = VK_NULL_HANDLE;
    m_rcasGradePipeline = VK_NULL_HANDLE;
    m_rcasPipelineLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_easuDescriptor = VK_NULL_HANDLE;
    m_rcasDescriptor = VK_NULL_HANDLE;
    m_easuLayout = VK_NULL_HANDLE;
    m_rcasLayout = VK_NULL_HANDLE;
    m_supported = false;
}
