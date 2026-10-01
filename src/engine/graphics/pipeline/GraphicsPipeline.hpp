#pragma once
#include <volk.h>

#include <cstdint>

// Vertex buffer bindings shared by every mesh pipeline, one per stream of
// PackedVertexStreams. Binding strides are dynamic state, so meshes without a
// skin stream bind a single zero influence with stride 0.
namespace mesh_vertex_binding {
constexpr uint32_t position = 0;
constexpr uint32_t attributes = 1;
constexpr uint32_t skin = 2;
constexpr uint32_t count = 3;
} // namespace mesh_vertex_binding

class GraphicsPipeline {
  public:
    GraphicsPipeline() = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    bool initGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                              VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                              VkDescriptorSetLayout materialLayout, VkShaderModule vertex, VkShaderModule fragment,
                              VkPipelineCache pipelineCache, bool doubleSided = false, bool blending = false,
                              const VkSpecializationInfo* fragmentSpecialization = nullptr);
    bool initShadowPipeline(VkDevice device, VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                            VkDescriptorSetLayout materialLayout, VkShaderModule vertex, VkShaderModule fragment,
                            VkPipelineCache pipelineCache);
    bool initOutlinePipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat, VkFormat depthFormat,
                             VkDescriptorSetLayout cameraLayout, VkShaderModule vertex, VkShaderModule fragment,
                             VkPipelineCache pipelineCache);
    bool initSelectionMaskPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                   VkFormat depthFormat, VkDescriptorSetLayout cameraLayout, VkShaderModule vertex,
                                   VkShaderModule fragment, VkPipelineCache pipelineCache);
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
