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
    glm::vec4 cameraPosition;
    glm::vec4 grid;
    glm::vec2 inverseResolution;
    float bloomStrength;
    float antialiasingMode;
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
    m_allocator = context.vmaAllocator();
    m_targets.resize(frameCount);
    return createDescriptors(frameCount) && createPipeline(outputFormat) && createTargets(context, extent, depthView);
}

bool PostProcessPass::createDescriptors(uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding bindings[]{{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = static_cast<uint32_t>(std::size(bindings)),
                                           .pBindings = bindings};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create post-process descriptor layout")) {
        return false;
    }
    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * 2};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = frameCount,
                                    .poolSizeCount = 1,
                                    .pPoolSizes = &poolSize};
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
    return checkVulkan(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline),
                       "Create post-process pipeline");
}

bool PostProcessPass::createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView) {
    m_extent = extent;
    for (Target& target : m_targets) {
        target.mipLevels = 1 + static_cast<uint32_t>(std::floor(std::log2(std::max(extent.width, extent.height))));
        VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = sceneFormat,
                                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                                .mipLevels = target.mipLevels,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
                                                        .levelCount = target.mipLevels,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = 1}};
        if (!checkVulkan(vkCreateImageView(context.device(), &view, nullptr, &target.view), "Create HDR scene view")) {
            return false;
        }
        const VkDescriptorImageInfo images[]{{.sampler = m_colorSampler,
                                              .imageView = target.view,
                                              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {.sampler = m_depthSampler,
                                              .imageView = depthView,
                                              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
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
                                             .pImageInfo = &images[1]}};
        vkUpdateDescriptorSets(context.device(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
    }
    return true;
}

void PostProcessPass::destroyTargets(const VulkanContext& context) {
    for (Target& target : m_targets) {
        if (target.view) {
            vkDestroyImageView(context.device(), target.view, nullptr);
        }
        if (target.image) {
            vmaDestroyImage(context.vmaAllocator(), target.image, target.allocation);
        }
        target.image = VK_NULL_HANDLE;
        target.view = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
        target.initialized = false;
        target.mipsInitialized = false;
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
                 target.initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 target.initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

bool PostProcessPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage,
                             VkImageView outputView, VkImage depthImage, VkExtent2D extent,
                             const RenderSettings& settings, const glm::mat4& inverseViewProjection,
                             const glm::vec3& cameraPosition, const std::optional<GridOverlay>& gridOverlay,
                             VkQueryPool timestampQueries, double& bloomCpuMilliseconds,
                             VkImageLayout outputOldLayout) {
    const auto bloomStart = std::chrono::steady_clock::now();
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 6);
    }
    Target& target = m_targets[frameIndex];
    if (settings.bloom != BloomQuality::Off && target.mipLevels > 1) {
        imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        uint32_t sourceWidth = extent.width;
        uint32_t sourceHeight = extent.height;
        for (uint32_t mip = 1; mip < target.mipLevels; ++mip) {
            imageBarrier(
                commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                target.mipsInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                target.mipsInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, target.mipsInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, mip);
            const uint32_t destinationWidth = std::max(sourceWidth / 2, 1u);
            const uint32_t destinationHeight = std::max(sourceHeight / 2, 1u);
            VkImageBlit blit{
                .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .mipLevel = mip - 1,
                                   .baseArrayLayer = 0,
                                   .layerCount = 1},
                .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(sourceWidth), static_cast<int32_t>(sourceHeight), 1}},
                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .mipLevel = mip,
                                   .baseArrayLayer = 0,
                                   .layerCount = 1},
                .dstOffsets = {{0, 0, 0},
                               {static_cast<int32_t>(destinationWidth), static_cast<int32_t>(destinationHeight), 1}}};
            vkCmdBlitImage(commandBuffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mip - 1);
            imageBarrier(
                commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                mip + 1 < target.mipLevels ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                mip + 1 < target.mipLevels ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                mip + 1 < target.mipLevels ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mip);
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        target.mipsInitialized = true;
    } else {
        imageBarrier(commandBuffer, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 7);
    }
    bloomCpuMilliseconds =
        settings.bloom == BloomQuality::Off
            ? 0.0
            : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bloomStart).count();
    target.initialized = true;
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &target.descriptor,
                            0, nullptr);
    const PostSettings postSettings{
        .inverseViewProjection = inverseViewProjection,
        .cameraPosition = {cameraPosition, 1.0f},
        .grid = gridOverlay ? glm::vec4(gridOverlay->planeHeight, gridOverlay->spacing, gridOverlay->fadeDistance, 1.0f)
                            : glm::vec4(0.0f),
        .inverseResolution = {1.0f / extent.width, 1.0f / extent.height},
        .bloomStrength = settings.bloom == BloomQuality::Off ? 0.0f : 0.10f,
        .antialiasingMode = static_cast<float>(settings.antialiasing),
    };
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(postSettings),
                       &postSettings);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
    return true;
}

VkImage PostProcessPass::sceneImage(uint32_t frameIndex) const {
    return m_targets[frameIndex].image;
}

VkImageView PostProcessPass::sceneView(uint32_t frameIndex) const {
    return m_targets[frameIndex].view;
}

void PostProcessPass::shutdown(const VulkanContext& context) {
    destroyTargets(context);
    if (m_pipeline) {
        vkDestroyPipeline(context.device(), m_pipeline, nullptr);
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
    m_pipelineLayout = VK_NULL_HANDLE;
    m_colorSampler = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}
