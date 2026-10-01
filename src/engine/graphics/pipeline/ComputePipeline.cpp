#include "ComputePipeline.hpp"

#include "../vulkan/VulkanCheck.hpp"

bool ComputePipeline::init(VkDevice device, VkDescriptorSetLayout descriptorLayout, VkShaderModule shader,
                           uint32_t pushConstantSize) {
    VkPushConstantRange pushConstant{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = pushConstantSize};
    VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &descriptorLayout,
                                          .pushConstantRangeCount = pushConstantSize == 0 ? 0u : 1u,
                                          .pPushConstantRanges = pushConstantSize == 0 ? nullptr : &pushConstant};
    if (!checkVulkan(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout),
                     "Create compute pipeline layout")) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stage{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                          .module = shader,
                                          .pName = "main"};
    VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage, .layout = m_layout};
    if (!checkVulkan(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline),
                     "Create compute pipeline")) {
        shutdown(device);
        return false;
    }
    return true;
}

void ComputePipeline::shutdown(VkDevice device) {
    if (m_pipeline) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_layout) {
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    }
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}
