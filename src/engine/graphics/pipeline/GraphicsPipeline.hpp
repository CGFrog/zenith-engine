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

// How an opaque scene pipeline uses the depth buffer.
enum class SceneDepthUsage {
    // Test against and write scene depth.
    TestAndWrite,
    // Shade only where the depth prepass stored this exact depth; no writes.
    // Requires bit-identical clip positions in both passes.
    EqualToPrepass,
};

class GraphicsPipeline {
  public:
    GraphicsPipeline() = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    // Blended pipelines never write depth and always use the ordinary test, so
    // depthUsage applies to opaque pipelines only.
    bool initGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                              VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                              VkDescriptorSetLayout materialLayout, VkShaderModule vertex, VkShaderModule fragment,
                              VkPipelineCache pipelineCache, bool doubleSided = false, bool blending = false,
                              const VkSpecializationInfo* fragmentSpecialization = nullptr,
                              SceneDepthUsage depthUsage = SceneDepthUsage::TestAndWrite);
    // Depth prepass: writes scene depth and one color target (normalFormat).
    // The vertex stage takes a mat4 push constant (the view matrix).
    bool initDepthNormalPipeline(VkDevice device, VkFormat normalFormat, VkFormat depthFormat,
                                 VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout materialLayout,
                                 VkShaderModule vertex, VkShaderModule fragment, VkPipelineCache pipelineCache,
                                 bool doubleSided);
    bool initShadowPipeline(VkDevice device, VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                            VkDescriptorSetLayout materialLayout, VkShaderModule vertex, VkShaderModule fragment,
                            VkPipelineCache pipelineCache);
    bool initOutlinePipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat, VkFormat depthFormat,
                             VkDescriptorSetLayout cameraLayout, VkShaderModule vertex, VkShaderModule fragment,
                             VkPipelineCache pipelineCache);
    bool initSelectionMaskPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                   VkFormat depthFormat, VkDescriptorSetLayout cameraLayout, VkShaderModule vertex,
                                   VkShaderModule fragment, VkPipelineCache pipelineCache);
    // Full-screen background at the far plane: no vertex input, depth tested
    // against the cleared value without writing, scene alpha left untouched.
    bool initSkyboxPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat, VkFormat depthFormat,
                            VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout environmentLayout,
                            VkShaderModule vertex, VkShaderModule fragment, VkPipelineCache pipelineCache,
                            uint32_t fragmentPushConstantSize);
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
