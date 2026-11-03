#include "DebugDrawPass.hpp"

#include "../Rendering.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>

namespace {
// Mirrors DebugLineSettings in shaders/include/debug_line.glsl.
struct DebugLinePushConstants {
    glm::mat4 viewProjection{1.0f};
    glm::vec4 depthTerms{0.0f};
    glm::vec4 target{0.0f};
};

static_assert(sizeof(DebugLinePushConstants) == 96);

// How far, relative to its distance, a depth-tested line may lie behind the
// scene surface and still show. Covers lines drawn on surfaces, whose depth
// was rasterized with TAA jitter and possibly at a lower resolution.
constexpr float relativeDepthBias = 0.004f;
constexpr VkDeviceSize minimumVertexBufferBytes = 64 * 1024;
constexpr uint32_t setsPerFrame = RenderRuntime::maximumViews;

bool isSrgbFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

// Orders the colour writes of the post-processing pass that finished each
// output before the lines blend over it. The layouts do not change, so one
// memory barrier covers every view.
void colorAttachmentBarrier(VkCommandBuffer commandBuffer) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   .dstAccessMask =
                                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// Where one list's vertices start in the frame's vertex buffer.
struct ListRange {
    uint32_t first{0};
    uint32_t count{0};
};
} // namespace

bool DebugDrawPass::init(const VulkanContext& context, VkFormat colorFormat, uint32_t frameCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_pipelineCache = context.pipelineCache();
    m_log = context.log();
    m_linearizeColors = isSrgbFormat(colorFormat);
    m_vertexBuffers.assign(frameCount, {});
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_depthSampler), "Create debug-draw sampler") &&
           createDescriptors(frameCount) && createPipeline(colorFormat);
}

bool DebugDrawPass::createDescriptors(uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding binding{.binding = 0,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               .descriptorCount = 1,
                                               .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkDescriptorSetLayoutCreateInfo layout{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_setLayout),
                     "Create debug-draw descriptor layout")) {
        return false;
    }
    const uint32_t setCount = frameCount * setsPerFrame;
    const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = setCount};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = setCount,
                                          .poolSizeCount = 1,
                                          .pPoolSizes = &poolSize};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool), "Create debug-draw descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(setsPerFrame, m_setLayout);
    m_depthSets.assign(frameCount, std::vector<VkDescriptorSet>(setsPerFrame, VK_NULL_HANDLE));
    for (std::vector<VkDescriptorSet>& sets : m_depthSets) {
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = m_pool,
                                                   .descriptorSetCount = setsPerFrame,
                                                   .pSetLayouts = layouts.data()};
        if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                         "Allocate debug-draw descriptors")) {
            return false;
        }
    }
    return true;
}

bool DebugDrawPass::createPipeline(VkFormat colorFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "debug_line.vert.spv") || !fragment.load(shaders / "debug_line.frag.spv")) {
        return false;
    }
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   .offset = 0,
                                   .size = sizeof(DebugLinePushConstants)};
    const VkPipelineLayoutCreateInfo pipelineLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .setLayoutCount = 1,
                                                    .pSetLayouts = &m_setLayout,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push};
    if (!checkVulkan(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_pipelineLayout),
                     "Create debug-draw pipeline layout")) {
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
    const VkVertexInputBindingDescription vertexBinding{
        .binding = 0, .stride = sizeof(DebugLineVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[]{
        {.location = 0,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(DebugLineVertex, position))},
        {.location = 1,
         .binding = 0,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = static_cast<uint32_t>(offsetof(DebugLineVertex, color))}};
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBinding,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes)),
        .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST};
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
    // Straight alpha over the output; its alpha channel is left untouched.
    const VkPipelineColorBlendAttachmentState colorAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &colorAttachment};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = static_cast<uint32_t>(std::size(states)),
                                                   .pDynamicStates = states};
    const VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                  .colorAttachmentCount = 1,
                                                  .pColorAttachmentFormats = &colorFormat};
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
                       "Create debug-draw pipeline");
}

bool DebugDrawPass::reserveVertices(VertexBuffer& buffer, VkDeviceSize bytes) {
    if (bytes <= buffer.capacity) {
        return true;
    }
    // The slot's previous submission retired, so its buffer can go now.
    destroy(buffer);
    const VkDeviceSize capacity = std::bit_ceil(std::max(bytes, minimumVertexBufferBytes));
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo details{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &info, &allocation, &buffer.buffer, &buffer.allocation, &details),
                     "Create debug-draw vertex buffer")) {
        m_log.error("Could not grow the debug-draw vertex buffer to " + std::to_string(capacity) + " bytes");
        buffer = {};
        return false;
    }
    buffer.mapped = details.pMappedData;
    buffer.capacity = capacity;
    return true;
}

void DebugDrawPass::destroy(VertexBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

bool DebugDrawPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const DebugDraw& shared,
                           std::span<const DebugDrawPassView> views) {
    if (views.empty()) {
        return true;
    }
    if (frameIndex >= m_vertexBuffers.size() || views.size() > setsPerFrame) {
        m_log.error("Debug draw got frame slot " + std::to_string(frameIndex) + " with " +
                    std::to_string(views.size()) + " views, beyond the pass's capacity");
        return false;
    }
    // Upload layout: shared tested, shared on-top, then each view's tested
    // and on-top lines.
    constexpr std::array<DebugDepth, 2> depthModes{DebugDepth::Tested, DebugDepth::AlwaysOnTop};
    size_t vertexCount = 0;
    const auto countList = [&](const DebugDraw* lines) {
        if (lines != nullptr) {
            for (DebugDepth depth : depthModes) {
                vertexCount += lines->lineVertices(depth).size();
            }
        }
    };
    countList(&shared);
    for (const DebugDrawPassView& view : views) {
        countList(view.lines);
    }
    if (vertexCount == 0) {
        return true;
    }
    VertexBuffer& buffer = m_vertexBuffers[frameIndex];
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(vertexCount) * sizeof(DebugLineVertex);
    if (!reserveVertices(buffer, bytes)) {
        return false;
    }
    auto* destination = static_cast<DebugLineVertex*>(buffer.mapped);
    uint32_t written = 0;
    const auto upload = [&](const DebugDraw* lines, DebugDepth depth) {
        ListRange range{.first = written};
        if (lines != nullptr) {
            const std::span<const DebugLineVertex> vertices = lines->lineVertices(depth);
            std::memcpy(destination + written, vertices.data(), vertices.size_bytes());
            range.count = static_cast<uint32_t>(vertices.size());
            written += range.count;
        }
        return range;
    };
    const std::array<ListRange, 2> sharedRanges{upload(&shared, DebugDepth::Tested),
                                                upload(&shared, DebugDepth::AlwaysOnTop)};
    std::array<std::array<ListRange, 2>, setsPerFrame> viewRanges{};
    for (size_t index = 0; index < views.size(); ++index) {
        viewRanges[index] = {upload(views[index].lines, DebugDepth::Tested),
                             upload(views[index].lines, DebugDepth::AlwaysOnTop)};
    }
    if (!checkVulkan(vmaFlushAllocation(m_allocator, buffer.allocation, 0, bytes), "Flush debug-draw vertices")) {
        return false;
    }

    colorAttachmentBarrier(commandBuffer);
    const VkDeviceSize vertexOffset = 0;
    for (size_t index = 0; index < views.size(); ++index) {
        const DebugDrawPassView& view = views[index];
        const VkDescriptorSet depthSet = m_depthSets[frameIndex][index];
        const VkDescriptorImageInfo depthImage{.sampler = m_depthSampler,
                                               .imageView = view.depthView,
                                               .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = depthSet,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &depthImage};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = view.colorView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {.offset = {0, 0}, .extent = view.extent},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color};
        vkCmdBeginRendering(commandBuffer, &rendering);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &depthSet, 0,
                                nullptr);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &buffer.buffer, &vertexOffset);
        const VkViewport viewport{.x = 0.0f,
                                  .y = 0.0f,
                                  .width = static_cast<float>(view.extent.width),
                                  .height = static_cast<float>(view.extent.height),
                                  .minDepth = 0.0f,
                                  .maxDepth = 1.0f};
        const VkRect2D scissor{.offset = {0, 0}, .extent = view.extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        // Reversed Z built from swapped planes: distance = B / (depth + A).
        const float range = view.farPlane - view.nearPlane;
        DebugLinePushConstants constants{
            .viewProjection = view.viewProjection,
            .depthTerms = {view.nearPlane / range, view.nearPlane * view.farPlane / range, relativeDepthBias, 0.0f},
            .target = {static_cast<float>(view.renderExtent.width) / static_cast<float>(view.extent.width),
                       static_cast<float>(view.renderExtent.height) / static_cast<float>(view.extent.height),
                       m_linearizeColors ? 1.0f : 0.0f, 0.0f}};
        for (size_t mode = 0; mode < depthModes.size(); ++mode) {
            const ListRange ranges[]{sharedRanges[mode], viewRanges[index][mode]};
            if (ranges[0].count + ranges[1].count == 0) {
                continue;
            }
            constants.depthTerms.w = depthModes[mode] == DebugDepth::AlwaysOnTop ? 1.0f : 0.0f;
            vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                               &constants);
            for (const ListRange& listRange : ranges) {
                if (listRange.count != 0) {
                    vkCmdDraw(commandBuffer, listRange.count, 1, listRange.first, 0);
                }
            }
        }
        vkCmdEndRendering(commandBuffer);
    }
    return true;
}

uint64_t DebugDrawPass::allocatedBytes() const {
    uint64_t bytes = 0;
    for (const VertexBuffer& buffer : m_vertexBuffers) {
        bytes += buffer.capacity;
    }
    return bytes;
}

void DebugDrawPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    for (VertexBuffer& buffer : m_vertexBuffers) {
        destroy(buffer);
    }
    m_vertexBuffers.clear();
    m_depthSets.clear();
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    }
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_depthSampler, nullptr);
    }
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}
