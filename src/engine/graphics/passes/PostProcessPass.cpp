#include "PostProcessPass.hpp"
#include <chrono>

#include "engine/resources/ResourceLocator.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
struct PostSettings {
    glm::mat4 inverseViewProjection;
    glm::mat4 previousViewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 grid;
    glm::vec2 inverseResolution;
    glm::vec2 sceneScale;
    float bloomStrength;
    float antialiasingMode;
    float temporalHistoryValid;
    float ambientOcclusionMode;
    float reflectionMode;
    float tonemapOperator;
    glm::vec2 bloomUvScale;
    glm::vec2 bloomUvLimit;
};

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage, VkPipelineStageFlags2 destinationStage,
                  VkAccessFlags2 sourceAccess, VkAccessFlags2 destinationAccess, uint32_t baseMip = 0,
                  uint32_t levelCount = 1) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = sourceStage,
                                  .srcAccessMask = sourceAccess,
                                  .dstStageMask = destinationStage,
                                  .dstAccessMask = destinationAccess,
                                  .oldLayout = oldLayout,
                                  .newLayout = newLayout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = aspect,
                                                       .baseMipLevel = baseMip,
                                                       .levelCount = levelCount,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

bool PostProcessPass::init(const VulkanContext& context, VkFormat outputFormat, VkFormat, uint32_t frameCount,
                           VkExtent2D extent, VkImageView depthView) {
    m_device = context.device();
    m_pipelineCache = context.pipelineCache();
    m_allocator = context.vmaAllocator();
    m_targets.resize(frameCount);
    return m_bloomExposure.init(context) && createDescriptors(frameCount) && createPipeline(outputFormat) &&
           createEffectPipeline(shaderRoot() / "ssao.frag.spv", ambientOcclusionFormat, m_ambientOcclusionPipeline) &&
           createEffectPipeline(shaderRoot() / "ssr.frag.spv", sceneFormat, m_reflectionPipeline) &&
           createTargets(context, extent, depthView);
}

bool PostProcessPass::createDescriptors(uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding bindings[]{{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 2,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 3,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 4,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 5,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 6,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 7,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 8,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = static_cast<uint32_t>(std::size(bindings)),
                                           .pBindings = bindings};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create post-process descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize poolSizes[]{
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * 8},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = frameCount}};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = frameCount,
                                    .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
                                    .pPoolSizes = poolSizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool),
                     "Create post-process descriptor pool")) {
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    std::vector<VkDescriptorSet> descriptors(frameCount);
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = m_pool,
                                         .descriptorSetCount = frameCount,
                                         .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, descriptors.data()),
                     "Allocate post-process descriptors")) {
        return false;
    }
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_targets[i].descriptor = descriptors[i];
    }

    VkSamplerCreateInfo colorSampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                     .magFilter = VK_FILTER_LINEAR,
                                     .minFilter = VK_FILTER_LINEAR,
                                     .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                     .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                     .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                     .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                     .maxLod = 16.0f};
    VkSamplerCreateInfo depthSampler = colorSampler;
    depthSampler.magFilter = VK_FILTER_NEAREST;
    depthSampler.minFilter = VK_FILTER_NEAREST;
    return checkVulkan(vkCreateSampler(m_device, &colorSampler, nullptr, &m_colorSampler),
                       "Create post-process color sampler") &&
           checkVulkan(vkCreateSampler(m_device, &depthSampler, nullptr, &m_depthSampler),
                       "Create post-process depth sampler");
}

bool PostProcessPass::createPipeline(VkFormat outputFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "fullscreen.vert.spv") || !fragment.load(shaders / "post.frag.spv")) {
        return false;
    }
    VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(PostSettings)};
    VkPipelineLayoutCreateInfo pipelineLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &m_layout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &push};
    if (!checkVulkan(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_pipelineLayout),
                     "Create post-process pipeline layout")) {
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
    VkPipelineVertexInputStateCreateInfo vertexInput{.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{.sType =
                                                        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                     .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState colorAttachment{.colorWriteMask =
                                                            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1,
                                              .pAttachments = &colorAttachment};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                             .dynamicStateCount = static_cast<uint32_t>(std::size(states)),
                                             .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &outputFormat};
    VkGraphicsPipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
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
                                          .layout = m_pipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipeline, nullptr, &m_pipeline),
                       "Create post-process pipeline");
}

bool PostProcessPass::createEffectPipeline(const std::filesystem::path& fragmentPath, VkFormat outputFormat,
                                           VkPipeline& outputPipeline) {
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaderRoot() / "fullscreen.vert.spv") || !fragment.load(fragmentPath)) {
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
    VkPipelineVertexInputStateCreateInfo vertexInput{.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{.sType =
                                                        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                     .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState colorAttachment{.colorWriteMask =
                                                            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1,
                                              .pAttachments = &colorAttachment};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                             .dynamicStateCount = static_cast<uint32_t>(std::size(states)),
                                             .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &outputFormat};
    VkGraphicsPipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
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
                                          .layout = m_pipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipeline, nullptr, &outputPipeline),
                       "Create screen-space effect pipeline");
}

bool PostProcessPass::createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView) {
    m_extent = extent;
    if (!m_bloomExposure.createChain(context, extent)) {
        return false;
    }
    for (Target& target : m_targets) {
        VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = sceneFormat,
                                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!checkVulkan(
                vmaCreateImage(context.vmaAllocator(), &image, &allocation, &target.image, &target.allocation, nullptr),
                "Create HDR scene target")) {
            return false;
        }
        VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = target.image,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = sceneFormat,
                                   .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .baseMipLevel = 0,
                                                        .levelCount = 1,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1}};
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.view), "Create HDR scene view")) {
            return false;
        }
        VkImageCreateInfo motionImage = image;
        motionImage.format = motionFormat;
        motionImage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!checkVulkan(vmaCreateImage(context.vmaAllocator(), &motionImage, &allocation, &target.motionImage,
                                        &target.motionAllocation, nullptr),
                         "Create motion-vector target")) {
            return false;
        }
        view.image = target.motionImage;
        view.format = motionFormat;
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.motionView),
                         "Create motion-vector view")) {
            return false;
        }
        VkImageCreateInfo effectImage = motionImage;
        effectImage.format = ambientOcclusionFormat;
        if (!checkVulkan(vmaCreateImage(context.vmaAllocator(), &effectImage, &allocation,
                                        &target.ambientOcclusionImage, &target.ambientOcclusionAllocation, nullptr),
                         "Create ambient-occlusion target")) {
            return false;
        }
        view.image = target.ambientOcclusionImage;
        view.format = ambientOcclusionFormat;
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.ambientOcclusionView),
                         "Create ambient-occlusion view")) {
            return false;
        }
        effectImage.format = sceneFormat;
        if (!checkVulkan(vmaCreateImage(context.vmaAllocator(), &effectImage, &allocation, &target.reflectionImage,
                                        &target.reflectionAllocation, nullptr),
                         "Create reflection target")) {
            return false;
        }
        view.image = target.reflectionImage;
        view.format = sceneFormat;
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.reflectionView),
                         "Create reflection view")) {
            return false;
        }
    }
    for (uint32_t index = 0; index < m_targets.size(); ++index) {
        Target& target = m_targets[index];
        const Target& history = m_targets[(index + m_targets.size() - 1) % m_targets.size()];
        const VkDescriptorImageInfo images[]{
            {.sampler = m_colorSampler,
             .imageView = target.view,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = m_depthSampler, .imageView = depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {.sampler = m_colorSampler,
             .imageView = history.view,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = m_colorSampler,
             .imageView = target.motionView,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = m_colorSampler,
             .imageView = target.ambientOcclusionView,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = m_colorSampler,
             .imageView = target.reflectionView,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            // The previous frame's motion target; its alpha holds that frame's depth.
            {.sampler = m_colorSampler,
             .imageView = history.motionView,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            // The bloom chain stays in GENERAL for compute writes.
            {.sampler = m_colorSampler,
             .imageView = m_bloomExposure.bloomView(),
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL}};
        const VkDescriptorBufferInfo exposure{
            .buffer = m_bloomExposure.exposureBuffer(), .offset = 0, .range = BloomExposurePass::exposureBufferSize};
        const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[0]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 1,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[1]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 2,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[2]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 3,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[3]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 4,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[4]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 5,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[5]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 6,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[6]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 7,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &images[7]},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = target.descriptor,
                                             .dstBinding = 8,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .pBufferInfo = &exposure}};
        vkUpdateDescriptorSets(context.device(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
    }
    return true;
}

void PostProcessPass::destroyTargets(const VulkanContext& context) {
    m_bloomExposure.destroyChain(context);
    for (Target& target : m_targets) {
        if (target.view) {
            vkDestroyImageView(context.device(), target.view, nullptr);
        }
        if (target.image) {
            vmaDestroyImage(context.vmaAllocator(), target.image, target.allocation);
        }
        if (target.motionView) {
            vkDestroyImageView(context.device(), target.motionView, nullptr);
        }
        if (target.motionImage) {
            vmaDestroyImage(context.vmaAllocator(), target.motionImage, target.motionAllocation);
        }
        if (target.ambientOcclusionView) {
            vkDestroyImageView(context.device(), target.ambientOcclusionView, nullptr);
        }
        if (target.ambientOcclusionImage) {
            vmaDestroyImage(context.vmaAllocator(), target.ambientOcclusionImage, target.ambientOcclusionAllocation);
        }
        if (target.reflectionView) {
            vkDestroyImageView(context.device(), target.reflectionView, nullptr);
        }
        if (target.reflectionImage) {
            vmaDestroyImage(context.vmaAllocator(), target.reflectionImage, target.reflectionAllocation);
        }
        target.image = VK_NULL_HANDLE;
        target.view = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
        target.initialized = false;
        target.motionImage = VK_NULL_HANDLE;
        target.motionView = VK_NULL_HANDLE;
        target.motionAllocation = VK_NULL_HANDLE;
        target.motionInitialized = false;
        target.ambientOcclusionImage = VK_NULL_HANDLE;
        target.ambientOcclusionView = VK_NULL_HANDLE;
        target.ambientOcclusionAllocation = VK_NULL_HANDLE;
        target.reflectionImage = VK_NULL_HANDLE;
        target.reflectionView = VK_NULL_HANDLE;
        target.reflectionAllocation = VK_NULL_HANDLE;
        target.effectsInitialized = false;
    }
}

bool PostProcessPass::resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView) {
    destroyTargets(context);
    return createTargets(context, extent, depthView);
}

void PostProcessPass::prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    Target& target = m_targets[frameIndex];
    imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                 target.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 // Bloom and exposure sample the scene from compute before post reads it.
                 target.initialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                    : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 target.initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(
        commandBuffer, target.motionImage, VK_IMAGE_ASPECT_COLOR_BIT,
        target.motionInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        target.motionInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        target.motionInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

bool PostProcessPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage,
                             VkImageView outputView, VkImage depthImage, VkExtent2D extent, VkExtent2D internalExtent,
                             const RenderSettings& settings, const glm::mat4& inverseViewProjection,
                             const glm::mat4& previousViewProjection, bool temporalHistoryValid,
                             const glm::vec3& cameraPosition, const std::optional<GridOverlay>& gridOverlay,
                             VkQueryPool timestampQueries, double& bloomCpuMilliseconds,
                             VkImageLayout outputOldLayout) {
    Target& target = m_targets[frameIndex];
    recordBloomAndExposure(commandBuffer, target, internalExtent, settings, timestampQueries, bloomCpuMilliseconds);
    target.initialized = true;
    imageBarrier(commandBuffer, target.motionImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.motionInitialized = true;
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const BloomSampling bloomSampling = m_bloomExposure.bloomSampling(internalExtent);
    const PostSettings postSettings{
        .inverseViewProjection = inverseViewProjection,
        .previousViewProjection = previousViewProjection,
        .cameraPosition = {cameraPosition, 1.0f},
        .grid = gridOverlay ? glm::vec4(gridOverlay->planeHeight, gridOverlay->spacing, gridOverlay->fadeDistance, 1.0f)
                            : glm::vec4(0.0f),
        .inverseResolution = {1.0f / extent.width, 1.0f / extent.height},
        .sceneScale = {static_cast<float>(internalExtent.width) / extent.width,
                       static_cast<float>(internalExtent.height) / extent.height},
        .bloomStrength = settings.bloom == BloomQuality::Off ? 0.0f : settings.bloomIntensity,
        .antialiasingMode = static_cast<float>(settings.antialiasing),
        .temporalHistoryValid = temporalHistoryValid ? 1.0f : 0.0f,
        .ambientOcclusionMode = static_cast<float>(settings.ambientOcclusion),
        .reflectionMode = static_cast<float>(settings.reflections),
        .tonemapOperator = static_cast<float>(settings.tonemap),
        .bloomUvScale = bloomSampling.uvScale,
        .bloomUvLimit = bloomSampling.uvLimit,
    };
    const auto recordEffect = [&](VkImage image, VkImageView view, VkPipeline pipeline, VkExtent2D effectExtent,
                                  bool enabled, float disabledClearValue, uint32_t firstTimestamp,
                                  uint32_t lastTimestamp) {
        imageBarrier(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                     target.effectsInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     target.effectsInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                               : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     target.effectsInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, firstTimestamp);
        }
        VkRenderingAttachmentInfo effectAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = enabled ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {
                .color = {{disabledClearValue, disabledClearValue, disabledClearValue, disabledClearValue}}}};
        VkRenderingInfo effectRendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {.offset = {0, 0}, .extent = effectExtent},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &effectAttachment};
        vkCmdBeginRendering(commandBuffer, &effectRendering);
        VkViewport effectViewport{.x = 0.0f,
                                  .y = 0.0f,
                                  .width = static_cast<float>(effectExtent.width),
                                  .height = static_cast<float>(effectExtent.height),
                                  .minDepth = 0.0f,
                                  .maxDepth = 1.0f};
        VkRect2D effectScissor{.offset = {0, 0}, .extent = effectExtent};
        vkCmdSetViewport(commandBuffer, 0, 1, &effectViewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &effectScissor);
        if (enabled) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                    &target.descriptor, 0, nullptr);
            vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(postSettings),
                               &postSettings);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        }
        vkCmdEndRendering(commandBuffer);
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries,
                                 lastTimestamp);
        }
        imageBarrier(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    const auto scaledEffectExtent = [&](bool halfResolution) {
        return halfResolution
                   ? VkExtent2D{std::max(1u, internalExtent.width / 2), std::max(1u, internalExtent.height / 2)}
                   : internalExtent;
    };
    recordEffect(target.ambientOcclusionImage, target.ambientOcclusionView, m_ambientOcclusionPipeline,
                 scaledEffectExtent(settings.ambientOcclusion == AmbientOcclusionQuality::HalfResolution),
                 settings.ambientOcclusion != AmbientOcclusionQuality::Off, 1.0f, 10, 11);
    recordEffect(target.reflectionImage, target.reflectionView, m_reflectionPipeline,
                 scaledEffectExtent(settings.reflections == ReflectionQuality::ScreenSpaceHalfResolution),
                 settings.reflections != ReflectionQuality::ProbesOnly, 0.0f, 12, 13);
    target.effectsInitialized = true;
    imageBarrier(commandBuffer, outputImage, VK_IMAGE_ASPECT_COLOR_BIT, outputOldLayout,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 outputOldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                                              : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 outputOldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = outputView,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {.offset = {0, 0}, .extent = extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color};
    vkCmdBeginRendering(commandBuffer, &rendering);
    VkViewport viewport{.x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<float>(extent.width),
                        .height = static_cast<float>(extent.height),
                        .minDepth = 0.0f,
                        .maxDepth = 1.0f};
    VkRect2D scissor{.offset = {0, 0}, .extent = extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 14);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &target.descriptor,
                            0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(postSettings),
                       &postSettings);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 15);
    }
    return true;
}

void PostProcessPass::recordBloomAndExposure(VkCommandBuffer commandBuffer, const Target& target,
                                             VkExtent2D internalExtent, const RenderSettings& settings,
                                             VkQueryPool timestampQueries, double& bloomCpuMilliseconds) {
    imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const BloomExposureInput input{.view = target.view, .imageExtent = m_extent, .validExtent = internalExtent};
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 17);
    }
    m_bloomExposure.recordExposure(commandBuffer, input, settings.exposure, exposureDeltaSeconds());
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 18);
    }
    const auto bloomStart = std::chrono::steady_clock::now();
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 6);
    }
    if (settings.bloom != BloomQuality::Off) {
        m_bloomExposure.recordBloom(commandBuffer, input, settings.bloomFilterRadius);
    } else {
        m_bloomExposure.initializeBloomLayout(commandBuffer);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 7);
    }
    bloomCpuMilliseconds =
        settings.bloom == BloomQuality::Off
            ? 0.0
            : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bloomStart).count();
}

// Exposure adapts in wall-clock time between recorded frames, independent of
// the simulation step (which is zero in render-only benchmarks). Long stalls
// are clamped so a hitch does not snap exposure.
float PostProcessPass::exposureDeltaSeconds() {
    constexpr float maximumDeltaSeconds = 0.25f;
    const auto now = std::chrono::steady_clock::now();
    const float delta =
        m_previousExposureTime ? std::chrono::duration<float>(now - *m_previousExposureTime).count() : 0.0f;
    m_previousExposureTime = now;
    return std::clamp(delta, 0.0f, maximumDeltaSeconds);
}

VkImage PostProcessPass::sceneImage(uint32_t frameIndex) const {
    return m_targets[frameIndex].image;
}

VkImageView PostProcessPass::sceneView(uint32_t frameIndex) const {
    return m_targets[frameIndex].view;
}

VkImage PostProcessPass::motionImage(uint32_t frameIndex) const {
    return m_targets[frameIndex].motionImage;
}

VkImageView PostProcessPass::motionView(uint32_t frameIndex) const {
    return m_targets[frameIndex].motionView;
}

void PostProcessPass::shutdown(const VulkanContext& context) {
    destroyTargets(context);
    m_bloomExposure.shutdown(context);
    m_previousExposureTime.reset();
    if (m_pipeline) {
        vkDestroyPipeline(context.device(), m_pipeline, nullptr);
    }
    if (m_ambientOcclusionPipeline) {
        vkDestroyPipeline(context.device(), m_ambientOcclusionPipeline, nullptr);
    }
    if (m_reflectionPipeline) {
        vkDestroyPipeline(context.device(), m_reflectionPipeline, nullptr);
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(context.device(), m_pipelineLayout, nullptr);
    }
    if (m_colorSampler) {
        vkDestroySampler(context.device(), m_colorSampler, nullptr);
    }
    if (m_depthSampler) {
        vkDestroySampler(context.device(), m_depthSampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(context.device(), m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(context.device(), m_layout, nullptr);
    }
    m_pipeline = VK_NULL_HANDLE;
    m_ambientOcclusionPipeline = VK_NULL_HANDLE;
    m_reflectionPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_colorSampler = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}
