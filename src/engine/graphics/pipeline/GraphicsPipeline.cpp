#include "GraphicsPipeline.hpp"
#include "../../assets/Assets.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include <array>
#include <iterator>

namespace {
constexpr float rasterLineWidth = 1.0f;
constexpr const char* shaderEntryPoint = "main";

struct FixedFunctionState {
    VkVertexInputBindingDescription vertexBinding{
        .binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 2> vertexAttributes{
        {{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
         {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)}}};
    VkPipelineVertexInputStateCreateInfo vertexInput{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                     .vertexBindingDescriptionCount = 1,
                                                     .pVertexBindingDescriptions = &vertexBinding,
                                                     .vertexAttributeDescriptionCount =
                                                         static_cast<uint32_t>(vertexAttributes.size()),
                                                     .pVertexAttributeDescriptions = vertexAttributes.data()};
    VkPipelineInputAssemblyStateCreateInfo assembly{.sType =
                                                        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_BACK_BIT,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                  .lineWidth = rasterLineWidth};
    VkPipelineMultisampleStateCreateInfo multisample{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                     .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE,
                                                .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState colorAttachment{.blendEnable = VK_FALSE,
                                                        .colorWriteMask =
                                                            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1,
                                              .pAttachments = &colorAttachment};
    static constexpr VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                             .dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
                                             .pDynamicStates = dynamicStates};

    VkGraphicsPipelineCreateInfo pipelineInfo() const {
        return {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pVertexInputState = &vertexInput,
                .pInputAssemblyState = &assembly,
                .pViewportState = &viewport,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisample,
                .pDepthStencilState = &depth,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic};
    }
};

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule module) {
    return {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage,
            .module = module,
            .pName = shaderEntryPoint};
}
} // namespace

bool GraphicsPipeline::initGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                                            VkDescriptorSetLayout cameraLayout, VkShaderModule vertex,
                                            VkShaderModule fragment) {
    constexpr uint32_t pushConstantSize = sizeof(glm::mat4) + sizeof(glm::vec4);
    VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = pushConstantSize};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &cameraLayout,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout), "Create pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &colorFormat,
                                            .depthAttachmentFormat = depthFormat};
    const FixedFunctionState state{};
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline),
                       "Create graphics pipeline");
}

void GraphicsPipeline::shutdown(VkDevice device) {
    if (m_pipeline) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    }
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
}
