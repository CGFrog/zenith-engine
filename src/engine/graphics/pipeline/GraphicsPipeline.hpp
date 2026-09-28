#pragma once
#include <volk.h>

class GraphicsPipeline {
  public:
    GraphicsPipeline() = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    bool initGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                              VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout materialLayout,
                              VkShaderModule vertex, VkShaderModule fragment, bool doubleSided = false,
                              bool blending = false);
    bool initShadowPipeline(VkDevice device, VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                            VkDescriptorSetLayout materialLayout, VkShaderModule vertex, VkShaderModule fragment);
    bool initOutlinePipeline(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                             VkDescriptorSetLayout cameraLayout, VkShaderModule vertex, VkShaderModule fragment);
    bool initSelectionMaskPipeline(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                                   VkDescriptorSetLayout cameraLayout, VkShaderModule vertex, VkShaderModule fragment);
    void shutdown(VkDevice device);

    VkPipeline handle() const {
        return m_pipeline;
    }

    VkPipelineLayout layout() const {
        return m_pipelineLayout;
    }

  private:
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
};
