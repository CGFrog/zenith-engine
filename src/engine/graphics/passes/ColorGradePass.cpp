#include "ColorGradePass.hpp"

#include "../ColorGrading.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <glm/gtc/packing.hpp>

#include <array>
#include <cstring>

namespace {
// Mirrors ColorGradeSettings in shaders/color_grade.frag.
struct ColorGradePushConstants {
    glm::vec4 lutDomainMinimum; // w: LUT strength, zero skips the LUT
    glm::vec4 lutDomainMaximum; // w: lattice size
    glm::vec4 vignette;         // intensity (zero skips), smoothness, roundness
    glm::vec4 filmGrain;        // intensity (zero skips), cell size in pixels
    glm::vec2 outputSize;
    uint32_t frame;
    uint32_t padding;
};

static_assert(sizeof(ColorGradePushConstants) == 80);

// The placeholder LUT bound until a requested one loads; never sampled.
constexpr uint32_t placeholderLutSize = 2;

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

std::filesystem::path resolveLutPath(const std::string& path, const std::filesystem::path& assetRoot) {
    const std::filesystem::path requested(path);
    if (requested.is_absolute()) {
        return requested;
    }
    return (assetRoot.empty() ? ::assetRoot() : assetRoot) / requested;
}
} // namespace

bool ColorGradePass::init(const VulkanContext& context, VkFormat outputFormat, uint32_t frameCount, VkExtent2D extent) {
    m_log = context.log();
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_pipelineCache = context.pipelineCache();
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create colour-grade sampler") &&
           createDescriptors(frameCount) && createPipeline(outputFormat) && createDisplayTarget(extent);
}

bool ColorGradePass::createDescriptors(uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding bindings[]{{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                  {.binding = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = static_cast<uint32_t>(std::size(bindings)),
                                                 .pBindings = bindings};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create colour-grade descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .descriptorCount = frameCount * static_cast<uint32_t>(std::size(bindings))};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = frameCount,
                                          .poolSizeCount = 1,
                                          .pPoolSizes = &poolSize};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool),
                     "Create colour-grade descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    m_sets.assign(frameCount, VK_NULL_HANDLE);
    m_boundGenerations.assign(frameCount, 0);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    return checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_sets.data()),
                       "Allocate colour-grade descriptors");
}

bool ColorGradePass::createPipeline(VkFormat outputFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "fullscreen.vert.spv") || !fragment.load(shaders / "color_grade.frag.spv")) {
        return false;
    }
    const VkPushConstantRange push{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(ColorGradePushConstants)};
    const VkPipelineLayoutCreateInfo pipelineLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .setLayoutCount = 1,
                                                    .pSetLayouts = &m_layout,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push};
    if (!checkVulkan(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_pipelineLayout),
                     "Create colour-grade pipeline layout")) {
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
                                                  .pColorAttachmentFormats = &outputFormat};
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
                                                .layout = m_pipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipeline, nullptr, &m_pipeline),
                       "Create colour-grade pipeline");
}

bool ColorGradePass::createDisplayTarget(VkExtent2D extent) {
    m_extent = extent;
    m_displayInitialized = false;
    ++m_resourceGeneration;
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = displayFormat,
                                  .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_display.image, &m_display.allocation, nullptr),
                     "Create colour-grade display target")) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = m_display.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = displayFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_display.view),
                       "Create colour-grade display view");
}

bool ColorGradePass::resize(VkExtent2D extent) {
    destroy(m_display);
    return createDisplayTarget(extent);
}

void ColorGradePass::destroy(Image& image) const {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, image.image, image.allocation);
    }
    image = {};
}

bool ColorGradePass::uploadLut(VkCommandBuffer commandBuffer, uint64_t frameSerial, const ColorLut& lut) {
    const size_t texelCount = lut.texels.size();
    std::vector<uint16_t> halves(texelCount * 4);
    for (size_t index = 0; index < texelCount; ++index) {
        const glm::vec3& texel = lut.texels[index];
        halves[index * 4 + 0] = glm::packHalf1x16(texel.r);
        halves[index * 4 + 1] = glm::packHalf1x16(texel.g);
        halves[index * 4 + 2] = glm::packHalf1x16(texel.b);
        halves[index * 4 + 3] = glm::packHalf1x16(1.0f);
    }
    const VkDeviceSize byteCount = halves.size() * sizeof(uint16_t);
    const VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = byteCount, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo stagingAllocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                                    .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMemory = VK_NULL_HANDLE;
    VmaAllocationInfo stagingDetails{};
    if (!checkVulkan(
            vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocation, &staging, &stagingMemory, &stagingDetails),
            "Create colour LUT staging buffer")) {
        return false;
    }
    // The copy executes with this frame, so the buffer retires with it.
    m_releases.defer(frameSerial, [allocator = m_allocator, staging, stagingMemory] {
        vmaDestroyBuffer(allocator, staging, stagingMemory);
    });
    std::memcpy(stagingDetails.pMappedData, halves.data(), byteCount);
    if (!checkVulkan(vmaFlushAllocation(m_allocator, stagingMemory, 0, byteCount), "Flush colour LUT staging buffer")) {
        return false;
    }

    LutTexture texture{.size = lut.size, .domainMinimum = lut.domainMinimum, .domainMaximum = lut.domainMaximum};
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_3D,
                                  .format = lutFormat,
                                  .extent = {.width = lut.size, .height = lut.size, .depth = lut.size},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &image, &allocation, &texture.image.image, &texture.image.allocation, nullptr),
            "Create colour LUT texture")) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = texture.image.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_3D,
                                     .format = lutFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &texture.image.view), "Create colour LUT view")) {
        destroy(texture.image);
        return false;
    }
    imageBarrier(commandBuffer, texture.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // .cube order (red fastest, then green, then blue) is the texel order of a
    // tightly packed 3D image with x = red, y = green, z = blue.
    const VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                      .mipLevel = 0,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1},
                                 .imageExtent = {.width = lut.size, .height = lut.size, .depth = lut.size}};
    vkCmdCopyBufferToImage(commandBuffer, staging, texture.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(commandBuffer, texture.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // Frames recorded up to this one may still sample the old texture.
    if (m_lut.image.image != VK_NULL_HANDLE) {
        m_releases.defer(frameSerial, [this, retired = m_lut.image]() mutable { destroy(retired); });
    }
    m_lut = texture;
    ++m_resourceGeneration;
    return true;
}

bool ColorGradePass::updateLut(VkCommandBuffer commandBuffer, uint64_t frameSerial, const ColorLutSettings& settings,
                               const std::filesystem::path& assetRoot) {
    if (m_lut.image.image == VK_NULL_HANDLE &&
        !uploadLut(commandBuffer, frameSerial, identityColorLut(placeholderLutSize))) {
        return false;
    }
    const std::string_view path =
        color_grading::lutRequested(settings) ? std::string_view(settings.path) : std::string_view{};
    if (path == m_requestedPath && (path.empty() || assetRoot == m_requestedRoot)) {
        return true;
    }
    m_requestedPath = std::string(path);
    m_requestedRoot = assetRoot;
    m_lutLoaded = false;
    if (path.empty()) {
        return true;
    }
    ColorLut lut;
    if (const ColorLutResult loaded = loadColorLut(resolveLutPath(m_requestedPath, assetRoot), lut); !loaded) {
        // Reported once per requested path; the grade runs without the LUT.
        m_log.warning("Colour grading LUT unavailable, skipping it: " + loaded.error);
        return true;
    }
    if (!uploadLut(commandBuffer, frameSerial, lut)) {
        return false;
    }
    m_lutLoaded = true;
    return true;
}

bool ColorGradePass::prepare(VkCommandBuffer commandBuffer, uint64_t frameSerial, const RenderSettings& settings,
                             const std::filesystem::path& assetRoot, bool& active) {
    const uint64_t frameCount = m_sets.size();
    if (frameSerial > frameCount) {
        m_releases.collect(frameSerial - frameCount);
    }
    if (!updateLut(commandBuffer, frameSerial, settings.colorLut, assetRoot)) {
        return false;
    }
    active = (m_lutLoaded && color_grading::lutRequested(settings.colorLut)) ||
             color_grading::vignetteActive(settings.vignette) || color_grading::filmGrainActive(settings.filmGrain);
    return true;
}

void ColorGradePass::beginDisplayTarget(VkCommandBuffer commandBuffer) {
    // The previous frame's grade sampled the target; its contents are replaced.
    imageBarrier(commandBuffer, m_display.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 m_displayInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    m_displayInitialized = true;
}

void ColorGradePass::writeDescriptors(uint32_t frameIndex) {
    const VkDescriptorImageInfo images[]{
        {.sampler = m_sampler, .imageView = m_display.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = m_sampler, .imageView = m_lut.image.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_sets[frameIndex],
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &images[0]},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_sets[frameIndex],
                                         .dstBinding = 1,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &images[1]}};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
    m_boundGenerations[frameIndex] = m_resourceGeneration;
}

void ColorGradePass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView outputView,
                            VkExtent2D extent, const RenderSettings& settings, uint64_t frameSerial) {
    imageBarrier(commandBuffer, m_display.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // This slot's previous submission has retired, so its set may be rewritten.
    if (m_boundGenerations[frameIndex] != m_resourceGeneration) {
        writeDescriptors(frameIndex);
    }
    const bool lut = m_lutLoaded && color_grading::lutRequested(settings.colorLut);
    const bool vignette = color_grading::vignetteActive(settings.vignette);
    const bool grain = color_grading::filmGrainActive(settings.filmGrain);
    const ColorGradePushConstants constants{
        .lutDomainMinimum = {m_lut.domainMinimum, lut ? settings.colorLut.strength : 0.0f},
        .lutDomainMaximum = {m_lut.domainMaximum, static_cast<float>(m_lut.size)},
        .vignette = {vignette ? settings.vignette.intensity : 0.0f, settings.vignette.smoothness,
                     settings.vignette.roundness, 0.0f},
        .filmGrain = {grain ? settings.filmGrain.intensity : 0.0f, settings.filmGrain.size, 0.0f, 0.0f},
        .outputSize = {static_cast<float>(extent.width), static_cast<float>(extent.height)},
        .frame = static_cast<uint32_t>(frameSerial),
        .padding = 0};
    const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                          .imageView = outputView,
                                          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = extent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{.x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(extent.width),
                              .height = static_cast<float>(extent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    const VkRect2D scissor{.offset = {0, 0}, .extent = extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_sets[frameIndex],
                            0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
}

uint64_t ColorGradePass::allocatedBytes() const {
    // Both formats are RGBA16F: eight bytes per texel.
    constexpr uint64_t texelBytes = 8;
    const uint64_t lutTexels = static_cast<uint64_t>(m_lut.size) * m_lut.size * m_lut.size;
    return (static_cast<uint64_t>(m_extent.width) * m_extent.height + lutTexels) * texelBytes;
}

void ColorGradePass::shutdown() {
    m_releases.collect(UINT64_MAX);
    destroy(m_display);
    destroy(m_lut.image);
    m_lut = {};
    m_lutLoaded = false;
    m_requestedPath.clear();
    m_requestedRoot.clear();
    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    }
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_sets.clear();
    m_boundGenerations.clear();
}
