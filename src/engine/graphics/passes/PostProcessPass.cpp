#include "PostProcessPass.hpp"
#include <chrono>

#include "engine/resources/ResourceLocator.hpp"

#include "../resources/FrameTimestamps.hpp"
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
    m_outputFormat = outputFormat;
    m_allocator = context.vmaAllocator();
    m_targets.resize(frameCount);
    // The display pipeline tone-maps into the colour-grade stage's target
    // instead of the output when a grade effect is active.
    return m_bloomExposure.init(context) && createDescriptors(frameCount) && createPipelineLayout() &&
           createPipeline(outputFormat, m_pipeline) &&
           createPipeline(ColorGradePass::displayFormat, m_displayPipeline) &&
           m_temporalResolve.init(context, frameCount, extent) &&
           m_spatialUpscale.init(context, outputFormat, ColorGradePass::displayFormat, extent) &&
           m_reflections.init(context, extent) && m_motionBlur.init(context, frameCount, extent) &&
           m_depthOfField.init(context, frameCount, extent) &&
           m_colorGrade.init(context, outputFormat, frameCount, extent) && createTargets(context, extent, depthView);
}

bool PostProcessPass::createDescriptors(uint32_t frameCount) {
    // 0: HDR scene, 1: depth, 2: temporally resolved scene, 3: bloom mip 0,
    // 4: exposure state. Must match the set declared in shaders/post.frag.
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
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * 4},
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

bool PostProcessPass::createPipelineLayout() {
    VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(PostSettings)};
    VkPipelineLayoutCreateInfo pipelineLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &m_layout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &push};
    return checkVulkan(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_pipelineLayout),
                       "Create post-process pipeline layout");
}

bool PostProcessPass::createPipeline(VkFormat outputFormat, VkPipeline& output) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "fullscreen.vert.spv") || !fragment.load(shaders / "post.frag.spv")) {
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
    return checkVulkan(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipeline, nullptr, &output),
                       "Create post-process pipeline");
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
                                // Storage: screen-space reflections composite in place.
                                // Transfer source: TransmissionBackground copies it.
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
        effectImage.format = normalFormat;
        if (!checkVulkan(vmaCreateImage(context.vmaAllocator(), &effectImage, &allocation, &target.normalImage,
                                        &target.normalAllocation, nullptr),
                         "Create prepass normal target")) {
            return false;
        }
        view.image = target.normalImage;
        view.format = normalFormat;
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.normalView),
                         "Create prepass normal view")) {
            return false;
        }
    }
    for (Target& target : m_targets) {
        const VkDescriptorImageInfo images[]{
            {.sampler = m_colorSampler,
             .imageView = target.view,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = m_depthSampler, .imageView = depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            // The temporally resolved scene; record() binds the latest one.
            {.sampler = m_colorSampler,
             .imageView = target.view,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            // The bloom chain stays in GENERAL for compute writes.
            {.sampler = m_colorSampler,
             .imageView = m_bloomExposure.bloomView(),
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL}};
        const VkDescriptorBufferInfo exposure{
            .buffer = m_bloomExposure.exposureBuffer(), .offset = 0, .range = BloomExposurePass::exposureBufferSize};
        VkWriteDescriptorSet writes[std::size(images) + 1]{};
        for (uint32_t binding = 0; binding < std::size(images); ++binding) {
            writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = target.descriptor,
                               .dstBinding = binding,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &images[binding]};
        }
        writes[std::size(images)] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = target.descriptor,
                                     .dstBinding = static_cast<uint32_t>(std::size(images)),
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .pBufferInfo = &exposure};
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
        if (target.normalView) {
            vkDestroyImageView(context.device(), target.normalView, nullptr);
        }
        if (target.normalImage) {
            vmaDestroyImage(context.vmaAllocator(), target.normalImage, target.normalAllocation);
        }
        if (target.specularView) {
            vkDestroyImageView(context.device(), target.specularView, nullptr);
        }
        if (target.specularImage) {
            vmaDestroyImage(context.vmaAllocator(), target.specularImage, target.specularAllocation);
        }
        target.image = VK_NULL_HANDLE;
        target.view = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
        target.initialized = false;
        target.motionImage = VK_NULL_HANDLE;
        target.motionView = VK_NULL_HANDLE;
        target.motionAllocation = VK_NULL_HANDLE;
        target.motionInitialized = false;
        target.normalImage = VK_NULL_HANDLE;
        target.normalView = VK_NULL_HANDLE;
        target.normalAllocation = VK_NULL_HANDLE;
        target.normalInitialized = false;
        target.specularImage = VK_NULL_HANDLE;
        target.specularView = VK_NULL_HANDLE;
        target.specularAllocation = VK_NULL_HANDLE;
        target.specularInitialized = false;
        target.specularWritten = false;
    }
}

bool PostProcessPass::resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView) {
    destroyTargets(context);
    m_previousFrameIndex.reset();
    m_temporalResolve.resize(extent);
    m_spatialUpscale.resize(extent);
    m_reflections.resize(extent);
    m_motionBlur.resize(extent);
    m_depthOfField.resize(extent);
    return m_colorGrade.resize(extent) && createTargets(context, extent, depthView);
}

void PostProcessPass::prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    Target& target = m_targets[frameIndex];
    // Earlier frames sampled these targets from fragment shaders and from
    // compute: bloom, exposure and the temporal resolve (which also reads the
    // previous frame's motion target).
    constexpr VkPipelineStageFlags2 sampledStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                 target.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 target.initialized ? sampledStages : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 target.initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(commandBuffer, target.motionImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 target.motionInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 target.motionInitialized ? sampledStages : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 target.motionInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

bool PostProcessPass::tracesReflections(const SceneGeometryViews& geometry, ReflectionQuality reflections) {
    return reflections != ReflectionQuality::ProbesOnly && geometry.valid() && geometry.specular != VK_NULL_HANDLE;
}

bool PostProcessPass::recordReflections(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                        const SceneGeometryViews& geometry, ReflectionQuality reflections,
                                        const ScreenSpaceReflectionCamera& camera, VkQueryPool timestampQueries) {
    const Target& target = m_targets[frameIndex];
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 12);
    }
    if (tracesReflections(geometry, reflections)) {
        imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (!m_reflections.record(commandBuffer, target.view, geometry, reflections, camera)) {
            return false;
        }
        // The scene pass resumes and blends later surfaces over the result.
        imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 13);
    }
    return true;
}

bool PostProcessPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage,
                             VkImageView outputView, VkImage depthImage, VkExtent2D extent, VkExtent2D internalExtent,
                             const RenderSettings& settings, const glm::mat4& inverseViewProjection,
                             const glm::mat4& previousViewProjection, const TemporalResolveFrame& temporal,
                             const depth_of_field::CameraTerms& depthOfFieldCamera, const glm::vec3& cameraPosition,
                             const std::optional<GridOverlay>& gridOverlay, const PostProcessFrame& frame,
                             VkQueryPool timestampQueries, double& bloomCpuMilliseconds,
                             VkImageLayout outputOldLayout) {
    Target& target = m_targets[frameIndex];
    constexpr VkPipelineStageFlags2 sampledStages =
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 sampledStages, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.initialized = true;
    imageBarrier(commandBuffer, target.motionImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 sampledStages, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.motionInitialized = true;
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const bool temporalEnabled = settings.antialiasing == AntialiasingMode::Taa;
    // FSR 1 composites (anti-aliases and tone maps) at render resolution and
    // upsamples afterwards, so TAA resolves at render resolution too. Without
    // support it composites at output resolution as if the upscaler were Off.
    const bool spatialUpscale = settings.upscaler.mode == UpscalerMode::Fsr1 && m_spatialUpscale.supported();
    const VkExtent2D resolveExtent = spatialUpscale ? internalExtent : m_extent;
    if (!resolveTemporal(commandBuffer, frameIndex, internalExtent, resolveExtent, temporalEnabled, temporal,
                         timestampQueries)) {
        return false;
    }
    // With TAA, later passes read the resolved scene (output resolution, or
    // render resolution ahead of FSR 1); otherwise the raw HDR target within
    // its render extent. Depth of field and motion blur, in that order, each
    // replace it with their output.
    const SceneColor resolved = temporalEnabled ? SceneColor{.view = m_temporalResolve.resolvedView(),
                                                             .layout = TemporalResolvePass::resolvedLayout,
                                                             .validExtent = m_temporalResolve.resolvedExtent()}
                                                : SceneColor{.view = target.view,
                                                             .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                             .validExtent = internalExtent};
    std::optional<SceneColor> scene = focusScene(commandBuffer, frameIndex, resolved, internalExtent,
                                                 settings.depthOfField, depthOfFieldCamera, frame.timestamps);
    if (!scene) {
        return false;
    }
    if (!recordMotionBlur(commandBuffer, frameIndex, internalExtent, settings, temporal, frame.timestamps, *scene)) {
        return false;
    }
    // A render-resolution resolve reaches the composite through binding 0,
    // like an unfiltered scene (see PostSettings::antialiasingMode below).
    bindSceneColor(frameIndex, *scene, temporalEnabled && !spatialUpscale);
    const BloomExposureInput bloomInput{
        .view = scene->view, .imageExtent = m_extent, .validExtent = scene->validExtent, .layout = scene->layout};
    recordBloomAndExposure(commandBuffer, bloomInput, settings, timestampQueries, bloomCpuMilliseconds);
    const BloomSampling bloomSampling = m_bloomExposure.bloomSampling(bloomInput.validExtent);
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
        // A render-resolution resolve reaches the composite through binding 0
        // (bindCompositeScene), so it reads it like an unfiltered scene.
        .antialiasingMode = temporalEnabled && spatialUpscale ? static_cast<float>(AntialiasingMode::Off)
                                                              : static_cast<float>(settings.antialiasing),
        .temporalHistoryValid = temporal.historyValid ? 1.0f : 0.0f,
        .ambientOcclusionMode = static_cast<float>(settings.ambientOcclusion),
        .reflectionMode = static_cast<float>(settings.reflections),
        .tonemapOperator = static_cast<float>(settings.tonemap),
        .bloomUvScale = bloomSampling.uvScale,
        .bloomUvLimit = bloomSampling.uvLimit,
    };
    bool grading = false;
    if (!m_colorGrade.prepare(commandBuffer, frame.serial, settings, frame.assetRoot, grading)) {
        return false;
    }
    // Tone mapping writes the output; with FSR 1 the top-left render extent of
    // the upscaler's input target instead, or with a grade effect the grade's
    // display target. FSR then sharpens into whichever of those two the grade
    // leaves, and the grade writes the output last.
    VkImageView toneMapView = outputView;
    VkExtent2D toneMapExtent = extent;
    VkPipeline toneMapPipeline = m_pipeline;
    if (spatialUpscale) {
        toneMapView = m_spatialUpscale.prepareInput(commandBuffer);
        toneMapExtent = internalExtent;
        if (toneMapView == VK_NULL_HANDLE) {
            return false;
        }
    } else if (grading) {
        m_colorGrade.beginDisplayTarget(commandBuffer);
        toneMapView = m_colorGrade.displayView();
        toneMapPipeline = m_displayPipeline;
    } else {
        transitionOutput(commandBuffer, outputImage, outputOldLayout);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 14);
    }
    recordToneMap(commandBuffer, target.descriptor, toneMapView, toneMapPipeline, toneMapExtent, &postSettings);
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 15);
    }
    if (spatialUpscale) {
        if (grading) {
            m_colorGrade.beginDisplayTarget(commandBuffer);
            recordSpatialUpscale(commandBuffer, internalExtent, m_colorGrade.displayView(),
                                 ColorGradePass::displayFormat, settings, timestampQueries);
        } else {
            transitionOutput(commandBuffer, outputImage, outputOldLayout);
            recordSpatialUpscale(commandBuffer, internalExtent, outputView, m_outputFormat, settings,
                                 timestampQueries);
        }
    } else {
        SpatialUpscalePass::writeSkippedTimestamps(commandBuffer, timestampQueries);
    }
    frame.timestamps.write(FrameTimestamp::colorGradeBegin, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    if (grading) {
        transitionOutput(commandBuffer, outputImage, outputOldLayout);
        m_colorGrade.record(commandBuffer, frameIndex, outputView, extent, settings, frame.serial);
    }
    frame.timestamps.write(FrameTimestamp::colorGradeEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    m_previousFrameIndex = frameIndex;
    m_previousRenderExtent = internalExtent;
    return true;
}

// An undefined output is a freshly acquired swapchain image: the frame waits
// on the acquire semaphore at color-attachment output, so the layout
// transition must start from that stage to stay ordered after it.
void PostProcessPass::transitionOutput(VkCommandBuffer commandBuffer, VkImage outputImage,
                                       VkImageLayout outputOldLayout) {
    imageBarrier(commandBuffer, outputImage, VK_IMAGE_ASPECT_COLOR_BIT, outputOldLayout,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 outputOldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                              : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 outputOldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void PostProcessPass::recordToneMap(VkCommandBuffer commandBuffer, VkDescriptorSet descriptor, VkImageView target,
                                    VkPipeline pipeline, VkExtent2D extent, const void* postSettings) {
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = target,
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
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptor, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostSettings),
                       postSettings);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
}

void PostProcessPass::recordSpatialUpscale(VkCommandBuffer commandBuffer, VkExtent2D internalExtent,
                                           VkImageView rcasTarget, VkFormat rcasFormat, const RenderSettings& settings,
                                           VkQueryPool timestampQueries) {
    m_spatialUpscale.recordEasu(commandBuffer, internalExtent, timestampQueries);
    m_spatialUpscale.recordRcas(commandBuffer, rcasTarget, rcasFormat, settings.upscaler.sharpness, timestampQueries);
}

bool PostProcessPass::resolveTemporal(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkExtent2D internalExtent,
                                      VkExtent2D resolveExtent, bool enabled, const TemporalResolveFrame& temporal,
                                      VkQueryPool timestampQueries) {
    const Target& target = m_targets[frameIndex];
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueries, 19);
    }
    if (enabled) {
        // With a single frame slot the previous motion target was just
        // overwritten, so disocclusion cannot be tested against it.
        const bool previousAvailable = m_previousFrameIndex && *m_previousFrameIndex != frameIndex &&
                                       m_targets[*m_previousFrameIndex].motionInitialized;
        const Target& previous = previousAvailable ? m_targets[*m_previousFrameIndex] : target;
        if (!m_temporalResolve.record(
                commandBuffer, frameIndex,
                {.sceneColor = target.view,
                 .motion = target.motionView,
                 .previousMotion = previous.motionView,
                 .renderExtent = internalExtent,
                 .previousRenderExtent = previousAvailable ? m_previousRenderExtent : internalExtent,
                 .resolveExtent = resolveExtent,
                 .previousMotionValid = previousAvailable},
                temporal)) {
            return false;
        }
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 20);
    }
    return true;
}

bool PostProcessPass::recordMotionBlur(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkExtent2D internalExtent,
                                       const RenderSettings& settings, const TemporalResolveFrame& temporal,
                                       OptionalPassTimestamps& passTimestamps, SceneColor& scene) {
    // Measured every frame so the first blurred frame after enabling it sees
    // a real frame time.
    const float frameSeconds = recordedFrameSeconds(m_previousMotionBlurTime);
    if (settings.motionBlur == MotionBlurQuality::Off) {
        return true;
    }
    const motion_blur::FramePlan plan = motion_blur::planFrame(
        settings, {scene.validExtent.width, scene.validExtent.height}, m_extent.width, frameSeconds);
    passTimestamps.write(FrameTimestamp::motionBlurBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (!m_motionBlur.record(commandBuffer, frameIndex,
                             {.sceneColor = scene.view,
                              .sceneColorLayout = scene.layout,
                              .colorRegion = scene.validExtent,
                              .motion = m_targets[frameIndex].motionView,
                              .motionRegion = internalExtent,
                              .currentToPreviousClip = temporal.currentToPreviousClip},
                             plan)) {
        return false;
    }
    passTimestamps.write(FrameTimestamp::motionBlurEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    scene.view = m_motionBlur.blurredView();
    scene.layout = MotionBlurPass::blurredLayout;
    return true;
}

std::optional<PostProcessPass::SceneColor>
PostProcessPass::focusScene(VkCommandBuffer commandBuffer, uint32_t frameIndex, const SceneColor& scene,
                            VkExtent2D internalExtent, const DepthOfFieldSettings& settings,
                            const depth_of_field::CameraTerms& camera, OptionalPassTimestamps& passTimestamps) {
    if (settings.quality == DepthOfFieldQuality::Off) {
        return scene;
    }
    passTimestamps.write(FrameTimestamp::depthOfFieldBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    // The scene and motion targets were made readable by compute at the start
    // of record(), and the resolved history by the temporal resolve.
    const DepthOfFieldImages images{.color = scene.view,
                                    .colorLayout = scene.layout,
                                    .colorExtent = scene.validExtent,
                                    .motionDepth = m_targets[frameIndex].motionView,
                                    .depthExtent = internalExtent};
    if (!m_depthOfField.record(commandBuffer, frameIndex, images, settings, camera)) {
        return std::nullopt;
    }
    passTimestamps.write(FrameTimestamp::depthOfFieldEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    return SceneColor{.view = m_depthOfField.outputView(),
                      .layout = DepthOfFieldPass::outputLayout,
                      .validExtent = scene.validExtent};
}

void PostProcessPass::bindSceneColor(uint32_t frameIndex, const SceneColor& scene, bool temporalEnabled) {
    // The post set is not yet bound in this command buffer, and its frame slot's
    // previous submission has retired, so it may be rewritten here.
    const Target& target = m_targets[frameIndex];
    const VkDescriptorImageInfo placeholder{
        .sampler = m_colorSampler, .imageView = target.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo sceneImage{
        .sampler = m_colorSampler, .imageView = scene.view, .imageLayout = scene.layout};
    const VkDescriptorImageInfo& renderResolution = temporalEnabled ? placeholder : sceneImage;
    const VkDescriptorImageInfo& outputResolution = temporalEnabled ? sceneImage : placeholder;
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = target.descriptor,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &renderResolution},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = target.descriptor,
                                         .dstBinding = 2,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &outputResolution}};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

void PostProcessPass::recordBloomAndExposure(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                                             const RenderSettings& settings, VkQueryPool timestampQueries,
                                             double& bloomCpuMilliseconds) {
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
// the simulation step (which is zero in render-only benchmarks), unless a fixed
// frame time makes it deterministic. Long stalls are clamped so a hitch does
// not snap exposure.
float PostProcessPass::exposureDeltaSeconds() {
    constexpr float maximumDeltaSeconds = 0.25f;
    return std::clamp(recordedFrameSeconds(m_previousExposureTime), 0.0f, maximumDeltaSeconds);
}

float PostProcessPass::recordedFrameSeconds(std::optional<std::chrono::steady_clock::time_point>& previousTime) const {
    if (m_fixedFrameSeconds) {
        return *m_fixedFrameSeconds;
    }
    const auto now = std::chrono::steady_clock::now();
    const float delta = previousTime ? std::chrono::duration<float>(now - *previousTime).count() : 0.0f;
    previousTime = now;
    return delta;
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

void PostProcessPass::prepareNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    Target& target = m_targets[frameIndex];
    // The prepass clears the target, so earlier contents are discarded.
    constexpr VkPipelineStageFlags2 readers =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    imageBarrier(commandBuffer, target.normalImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 target.normalInitialized ? readers : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 target.normalInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    target.specularWritten = false;
}

bool PostProcessPass::ensureSpecularTarget(Target& target) {
    if (target.specularImage) {
        return true;
    }
    // Same size and lifetime as the normal target, so dynamic resolution
    // renders into its top-left like every other prepass output.
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = specularFormat,
                                  .extent = {.width = m_extent.width, .height = m_extent.height, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &target.specularImage, &target.specularAllocation,
                                    nullptr),
                     "Create prepass specular target")) {
        target.specularImage = VK_NULL_HANDLE;
        target.specularAllocation = VK_NULL_HANDLE;
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = target.specularImage,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = specularFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &target.specularView),
                     "Create prepass specular view")) {
        vmaDestroyImage(m_allocator, target.specularImage, target.specularAllocation);
        target.specularImage = VK_NULL_HANDLE;
        target.specularView = VK_NULL_HANDLE;
        target.specularAllocation = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool PostProcessPass::prepareSpecular(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    Target& target = m_targets[frameIndex];
    if (!ensureSpecularTarget(target)) {
        return false;
    }
    // The prepass clears the target; only the reflection composite reads it.
    imageBarrier(
        commandBuffer, target.specularImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        target.specularInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        target.specularInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    target.specularWritten = true;
    return true;
}

void PostProcessPass::finishNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    Target& target = m_targets[frameIndex];
    imageBarrier(commandBuffer, target.normalImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.normalInitialized = true;
    if (target.specularWritten) {
        imageBarrier(commandBuffer, target.specularImage, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.specularInitialized = true;
    }
}

VkImageView PostProcessPass::normalView(uint32_t frameIndex) const {
    return m_targets[frameIndex].normalView;
}

VkImageView PostProcessPass::specularView(uint32_t frameIndex) const {
    const Target& target = m_targets[frameIndex];
    return target.specularWritten ? target.specularView : VK_NULL_HANDLE;
}

uint64_t PostProcessPass::reflectionBytes() const {
    uint64_t bytes = m_reflections.targetBytes();
    for (const Target& target : m_targets) {
        if (target.specularImage) {
            // specularFormat is four bytes per texel.
            bytes += static_cast<uint64_t>(m_extent.width) * m_extent.height * 4;
        }
    }
    return bytes;
}

void PostProcessPass::shutdown(const VulkanContext& context) {
    destroyTargets(context);
    m_temporalResolve.shutdown();
    m_spatialUpscale.shutdown();
    m_reflections.shutdown();
    m_motionBlur.shutdown();
    m_depthOfField.shutdown();
    m_bloomExposure.shutdown(context);
    m_colorGrade.shutdown();
    m_previousExposureTime.reset();
    m_previousMotionBlurTime.reset();
    if (m_pipeline) {
        vkDestroyPipeline(context.device(), m_pipeline, nullptr);
    }
    if (m_displayPipeline) {
        vkDestroyPipeline(context.device(), m_displayPipeline, nullptr);
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
    m_displayPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_colorSampler = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}
