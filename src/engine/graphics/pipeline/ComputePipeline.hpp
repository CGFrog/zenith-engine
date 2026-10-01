#pragma once

#include <cstdint>
#include <volk.h>

class ComputePipeline {
  public:
    ComputePipeline() = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    bool init(VkDevice device, VkDescriptorSetLayout descriptorLayout, VkShaderModule shader,
              uint32_t pushConstantSize = 0);
    void shutdown(VkDevice device);

    VkPipeline handle() const {
        return m_pipeline;
    }

    VkPipelineLayout layout() const {
        return m_layout;
    }

  private:
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
};
