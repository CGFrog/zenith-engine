#include "GraphicsPipeline.hpp"
#include "../../assets/PackedVertexStreams.hpp"
#include "../DepthConvention.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include <array>
#include <cstddef>
#include <iterator>

namespace {
constexpr float rasterLineWidth = 1.0f;
// Depth bias of wireframe overlay edges, in the units of
// VkPipelineRasterizationStateCreateInfo (positive is toward the camera).
constexpr float wireframeDepthBiasConstant = 1.0f;
constexpr float wireframeDepthBiasSlope = 1.0f;
constexpr const char* shaderEntryPoint = "main";

// Shader locations: 0 position, 1 octahedral normal, 2 UV, 3 octahedral
// tangent with folded sign, 4 joints, 5 weights. See PackedVertexStreams.hpp.
constexpr std::array<VkVertexInputAttributeDescription, 6> meshVertexAttributes{
    {{.location = 0, .binding = mesh_vertex_binding::position, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
     {.location = 1,
      .binding = mesh_vertex_binding::attributes,
      .format = VK_FORMAT_R16G16_SNORM,
      .offset = offsetof(PackedVertexAttributes, normal)},
     {.location = 2,
      .binding = mesh_vertex_binding::attributes,
      .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = offsetof(PackedVertexAttributes, texCoord)},
     {.location = 3,
      .binding = mesh_vertex_binding::attributes,
      .format = VK_FORMAT_R16G16_SNORM,
      .offset = offsetof(PackedVertexAttributes, tangent)},
     {.location = 4,
      .binding = mesh_vertex_binding::skin,
      .format = VK_FORMAT_R16G16B16A16_UINT,
      .offset = offsetof(PackedSkinInfluence, joints)},
     {.location = 5,
      .binding = mesh_vertex_binding::skin,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .offset = offsetof(PackedSkinInfluence, weights)}}};

constexpr std::array<VkVertexInputBindingDescription, mesh_vertex_binding::count> meshVertexBindings{
    {{.binding = mesh_vertex_binding::position,
      .stride = vertex_packing::positionStride,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
     {.binding = mesh_vertex_binding::attributes,
      .stride = vertex_packing::attributeStride,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
     {.binding = mesh_vertex_binding::skin,
      .stride = vertex_packing::skinStride,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}}};

// Which shader inputs a mesh pipeline reads; depth-style passes skip the
// shading attributes so they fetch only the streams they need.
enum class MeshVertexInputs : uint8_t {
    Shaded,
    PositionTexCoordSkin,
    PositionSkin,
};

bool readsLocation(MeshVertexInputs inputs, uint32_t location) {
    switch (inputs) {
    case MeshVertexInputs::Shaded:
        return true;
    case MeshVertexInputs::PositionTexCoordSkin:
        return location == 0 || location == 2 || location >= 4;
    case MeshVertexInputs::PositionSkin:
        return location == 0 || location >= 4;
    }
    return false;
}

struct FixedFunctionState {
    explicit FixedFunctionState(MeshVertexInputs inputs) {
        std::array<bool, mesh_vertex_binding::count> usedBindings{};
        uint32_t attributeCount = 0;
        for (const VkVertexInputAttributeDescription& attribute : meshVertexAttributes) {
            if (readsLocation(inputs, attribute.location)) {
                vertexAttributes[attributeCount++] = attribute;
                usedBindings[attribute.binding] = true;
            }
        }
        uint32_t bindingCount = 0;
        for (const VkVertexInputBindingDescription& binding : meshVertexBindings) {
            if (usedBindings[binding.binding]) {
                vertexBindings[bindingCount++] = binding;
            }
        }
        vertexInput.vertexBindingDescriptionCount = bindingCount;
        vertexInput.vertexAttributeDescriptionCount = attributeCount;
    }

    // The create-info members point into this object.
    FixedFunctionState(const FixedFunctionState&) = delete;
    FixedFunctionState& operator=(const FixedFunctionState&) = delete;

    std::array<VkVertexInputBindingDescription, mesh_vertex_binding::count> vertexBindings{};
    std::array<VkVertexInputAttributeDescription, meshVertexAttributes.size()> vertexAttributes{};
    VkPipelineVertexInputStateCreateInfo vertexInput{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                     .pVertexBindingDescriptions = vertexBindings.data(),
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
    static constexpr VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                    VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
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

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule module,
                                            const VkSpecializationInfo* specialization = nullptr) {
    return {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage,
            .module = module,
            .pName = shaderEntryPoint,
            .pSpecializationInfo = specialization};
}
} // namespace

bool GraphicsPipeline::initGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                            VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                            VkDescriptorSetLayout materialLayout, VkShaderModule vertex,
                                            VkShaderModule fragment, VkPipelineCache pipelineCache, bool doubleSided,
                                            bool blending, const VkSpecializationInfo* fragmentSpecialization,
                                            SceneDepthUsage depthUsage) {
    // Per-object and per-material data live in storage buffers indexed by the
    // instance, so mesh pipelines take no push constants.
    const VkDescriptorSetLayout setLayouts[]{cameraLayout, materialLayout};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = static_cast<uint32_t>(std::size(setLayouts)),
                                      .pSetLayouts = setLayouts};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout), "Create pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{
        shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
        shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment, fragmentSpecialization)};
    const VkFormat colorFormats[]{colorFormat, auxiliaryColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 2,
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    FixedFunctionState state{MeshVertexInputs::Shaded};
    state.raster.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    state.depth.depthCompareOp = scene_depth::compareOp;
    if (!blending && depthUsage == SceneDepthUsage::EqualToPrepass) {
        state.depth.depthWriteEnable = VK_FALSE;
        state.depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    }
    if (blending) {
        state.depth.depthWriteEnable = VK_FALSE;
        state.colorAttachment.blendEnable = VK_TRUE;
        state.colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        state.colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    state.colorAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{
        state.colorAttachment,
        {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT}};
    state.blend.attachmentCount = 2;
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create graphics pipeline");
}

bool GraphicsPipeline::initWireframeOverlayPipeline(VkDevice device, VkFormat colorFormat,
                                                    VkFormat auxiliaryColorFormat, VkFormat depthFormat,
                                                    VkDescriptorSetLayout cameraLayout,
                                                    VkDescriptorSetLayout materialLayout, VkShaderModule vertex,
                                                    VkShaderModule fragment, VkPipelineCache pipelineCache,
                                                    bool doubleSided) {
    // Same layout as the shading pipelines, so the overlay binds their sets.
    const VkDescriptorSetLayout setLayouts[]{cameraLayout, materialLayout};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = static_cast<uint32_t>(std::size(setLayouts)),
                                      .pSetLayouts = setLayouts};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create wireframe pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{colorFormat, auxiliaryColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 2,
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    // The vertex stage is mesh.vert, which reads every stream.
    FixedFunctionState state{MeshVertexInputs::Shaded};
    state.raster.polygonMode = VK_POLYGON_MODE_LINE;
    state.raster.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    // Edge pixels may land on a neighbouring triangle slightly in front, so
    // edges move toward the camera (larger reversed-Z depth) by a depth step
    // plus their polygon's slope.
    state.raster.depthBiasEnable = VK_TRUE;
    state.raster.depthBiasConstantFactor = wireframeDepthBiasConstant;
    state.raster.depthBiasSlopeFactor = wireframeDepthBiasSlope;
    state.depth.depthWriteEnable = VK_FALSE;
    state.depth.depthCompareOp = scene_depth::compareOrEqualOp;
    state.colorAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{state.colorAttachment, {.colorWriteMask = 0}};
    state.blend.attachmentCount = static_cast<uint32_t>(std::size(colorAttachments));
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create wireframe pipeline");
}

bool GraphicsPipeline::initIdBufferPipeline(VkDevice device, VkFormat idFormat, VkFormat depthColorFormat,
                                            VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                            VkShaderModule vertex, VkShaderModule fragment,
                                            VkPipelineCache pipelineCache, uint32_t pushConstantSize) {
    const VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = pushConstantSize};
    const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1,
                                            .pSetLayouts = &cameraLayout,
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create id-buffer pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{idFormat, depthColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = static_cast<uint32_t>(std::size(colorFormats)),
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    FixedFunctionState state{MeshVertexInputs::PositionSkin};
    // Both faces: an object seen from inside still picks, and nearer surfaces
    // win the depth test anyway.
    state.raster.cullMode = VK_CULL_MODE_NONE;
    state.depth.depthCompareOp = scene_depth::compareOp;
    // Integer targets cannot blend.
    state.colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{state.colorAttachment, state.colorAttachment};
    state.blend.attachmentCount = static_cast<uint32_t>(std::size(colorAttachments));
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create id-buffer pipeline");
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

bool GraphicsPipeline::initShadowPipeline(VkDevice device, VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                          VkDescriptorSetLayout materialLayout, VkShaderModule vertex,
                                          VkShaderModule fragment, VkPipelineCache pipelineCache) {
    // x selects the cascade; masking reads the object's material record.
    VkPushConstantRange pushConstants{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(glm::vec4)};
    const VkDescriptorSetLayout layouts[]{cameraLayout, materialLayout};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = static_cast<uint32_t>(std::size(layouts)),
                                      .pSetLayouts = layouts,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create shadow pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .depthAttachmentFormat = depthFormat};
    FixedFunctionState state{MeshVertexInputs::PositionTexCoordSkin};
    state.raster.cullMode = VK_CULL_MODE_BACK_BIT;
    state.raster.depthBiasEnable = VK_TRUE;
    const VkDynamicState shadowDynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                               VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_CULL_MODE,
                                               VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    state.dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(shadowDynamicStates));
    state.dynamic.pDynamicStates = shadowDynamicStates;
    state.blend.attachmentCount = 0;
    state.blend.pAttachments = nullptr;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create shadow pipeline");
}

bool GraphicsPipeline::initOutlinePipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                           VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                           VkShaderModule vertex, VkShaderModule fragment,
                                           VkPipelineCache pipelineCache) {
    VkPushConstantRange pushConstants{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(glm::vec4)};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &cameraLayout,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create outline pipeline layout")) {
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{colorFormat, auxiliaryColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 2,
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    FixedFunctionState state{MeshVertexInputs::PositionSkin};
    state.depth.depthWriteEnable = VK_FALSE;
    state.depth.depthCompareOp = scene_depth::compareOrEqualOp;
    state.colorAttachment.blendEnable = VK_TRUE;
    state.colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    state.colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    state.colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    state.colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    state.colorAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{state.colorAttachment, {}};
    state.blend.attachmentCount = 2;
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create outline pipeline");
}

bool GraphicsPipeline::initSelectionMaskPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                                 VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                                 VkShaderModule vertex, VkShaderModule fragment,
                                                 VkPipelineCache pipelineCache) {
    VkPipelineLayoutCreateInfo layout{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &cameraLayout};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create selection-mask pipeline layout")) {
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{colorFormat, auxiliaryColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 2,
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    FixedFunctionState state{MeshVertexInputs::PositionSkin};
    state.depth.depthTestEnable = VK_FALSE;
    state.depth.depthWriteEnable = VK_FALSE;
    state.colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{state.colorAttachment, {}};
    state.blend.attachmentCount = 2;
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create selection-mask pipeline");
}

bool GraphicsPipeline::initSkyboxPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                          VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                          VkDescriptorSetLayout environmentLayout, VkShaderModule vertex,
                                          VkShaderModule fragment, VkPipelineCache pipelineCache,
                                          uint32_t fragmentPushConstantSize) {
    const VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = fragmentPushConstantSize};
    const VkDescriptorSetLayout setLayouts[]{cameraLayout, environmentLayout};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = static_cast<uint32_t>(std::size(setLayouts)),
                                      .pSetLayouts = setLayouts,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create skybox pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{colorFormat, auxiliaryColorFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 2,
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    // The vertex shader generates a full-screen triangle from gl_VertexIndex, so
    // the mesh vertex streams and their dynamic strides do not apply.
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
                                                        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                        .lineWidth = rasterLineWidth};
    const VkPipelineMultisampleStateCreateInfo multisample{.sType =
                                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    // Only pixels still at the cleared far-plane depth receive the background.
    const VkPipelineDepthStencilStateCreateInfo depth{.sType =
                                                          VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                      .depthTestEnable = VK_TRUE,
                                                      .depthWriteEnable = VK_FALSE,
                                                      .depthCompareOp = scene_depth::compareOrEqualOp};
    // Scene alpha carries the editor selection mask, so the color target keeps it.
    const VkPipelineColorBlendAttachmentState colorAttachments[]{
        {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT},
        {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT}};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount =
                                                        static_cast<uint32_t>(std::size(colorAttachments)),
                                                    .pAttachments = colorAttachments};
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
                                                   .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .pNext = &rendering,
                                            .stageCount = static_cast<uint32_t>(std::size(stages)),
                                            .pStages = stages,
                                            .pVertexInputState = &vertexInput,
                                            .pInputAssemblyState = &assembly,
                                            .pViewportState = &viewport,
                                            .pRasterizationState = &raster,
                                            .pMultisampleState = &multisample,
                                            .pDepthStencilState = &depth,
                                            .pColorBlendState = &blend,
                                            .pDynamicState = &dynamic,
                                            .layout = m_pipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create skybox pipeline");
}

bool GraphicsPipeline::initParticlePipeline(VkDevice device, VkFormat colorFormat, VkFormat motionFormat,
                                            VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                            VkDescriptorSetLayout particleLayout, VkShaderModule vertex,
                                            VkShaderModule fragment, VkPipelineCache pipelineCache,
                                            uint32_t vertexPushConstantSize) {
    const VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = vertexPushConstantSize};
    const VkDescriptorSetLayout setLayouts[]{cameraLayout, particleLayout};
    const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = static_cast<uint32_t>(std::size(setLayouts)),
                                            .pSetLayouts = setLayouts,
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create particle pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{colorFormat, motionFormat};
    const VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                  .colorAttachmentCount = 2,
                                                  .pColorAttachmentFormats = colorFormats,
                                                  .depthAttachmentFormat = depthFormat};
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
                                                        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                        .lineWidth = rasterLineWidth};
    const VkPipelineMultisampleStateCreateInfo multisample{.sType =
                                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{.sType =
                                                          VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                      .depthTestEnable = VK_TRUE,
                                                      .depthWriteEnable = VK_FALSE,
                                                      .depthCompareOp = scene_depth::compareOrEqualOp};
    // Scene alpha carries the editor selection mask, so the color target keeps
    // it; additive particles output zero alpha to the premultiplied blend.
    const VkPipelineColorBlendAttachmentState colorAttachments[]{
        {.blendEnable = VK_TRUE,
         .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .alphaBlendOp = VK_BLEND_OP_ADD,
         .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT},
        // Motion vectors and depth stay those of the opaque surface behind.
        {.blendEnable = VK_TRUE,
         .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
         .colorBlendOp = VK_BLEND_OP_MAX,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .alphaBlendOp = VK_BLEND_OP_MAX,
         .colorWriteMask = VK_COLOR_COMPONENT_B_BIT}};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount =
                                                        static_cast<uint32_t>(std::size(colorAttachments)),
                                                    .pAttachments = colorAttachments};
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
                                                   .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .pNext = &rendering,
                                            .stageCount = static_cast<uint32_t>(std::size(stages)),
                                            .pStages = stages,
                                            .pVertexInputState = &vertexInput,
                                            .pInputAssemblyState = &assembly,
                                            .pViewportState = &viewport,
                                            .pRasterizationState = &raster,
                                            .pMultisampleState = &multisample,
                                            .pDepthStencilState = &depth,
                                            .pColorBlendState = &blend,
                                            .pDynamicState = &dynamic,
                                            .layout = m_pipelineLayout};
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create particle pipeline");
}

bool GraphicsPipeline::initDepthNormalPipeline(VkDevice device, VkFormat normalFormat, VkFormat specularFormat,
                                               VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                               VkDescriptorSetLayout materialLayout, VkShaderModule vertex,
                                               VkShaderModule fragment, VkPipelineCache pipelineCache,
                                               bool doubleSided) {
    // The view matrix rotates normals into view space.
    const VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(glm::mat4)};
    const VkDescriptorSetLayout setLayouts[]{cameraLayout, materialLayout};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = static_cast<uint32_t>(std::size(setLayouts)),
                                      .pSetLayouts = setLayouts,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pushConstants};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipelineLayout),
                     "Create depth-normal pipeline layout")) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[]{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertex),
                                                   shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
    const VkFormat colorFormats[]{normalFormat, specularFormat};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = static_cast<uint32_t>(std::size(colorFormats)),
                                            .pColorAttachmentFormats = colorFormats,
                                            .depthAttachmentFormat = depthFormat};
    // Normal mapping and alpha masking read every stream the shading pass reads.
    FixedFunctionState state{MeshVertexInputs::Shaded};
    state.raster.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    state.depth.depthCompareOp = scene_depth::compareOp;
    const VkPipelineColorBlendAttachmentState colorAttachments[]{state.colorAttachment, state.colorAttachment};
    state.blend.attachmentCount = static_cast<uint32_t>(std::size(colorAttachments));
    state.blend.pAttachments = colorAttachments;
    VkGraphicsPipelineCreateInfo info = state.pipelineInfo();
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.layout = m_pipelineLayout;
    return checkVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &m_pipeline),
                       "Create depth-normal pipeline");
}
