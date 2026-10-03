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
    // Wireframe view shading overlay: the mesh pipelines' layout and vertex
    // input with polygons rasterized as lines (needs fillModeNonSolid). Edges
    // are depth tested, biased toward the camera, against the surfaces already
    // drawn without writing depth; only the scene color's RGB is written, so
    // the selection mask (scene alpha) and motion vectors stay the surface's.
    bool initWireframeOverlayPipeline(VkDevice device, VkFormat colorFormat, VkFormat auxiliaryColorFormat,
                                      VkFormat depthFormat, VkDescriptorSetLayout cameraLayout,
                                      VkDescriptorSetLayout materialLayout, VkShaderModule vertex,
                                      VkShaderModule fragment, VkPipelineCache pipelineCache, bool doubleSided);
    // Depth prepass: writes scene depth and two color targets, normals
    // (normalFormat) then specular reflectance (specularFormat). The vertex
    // stage takes a mat4 push constant (the view matrix).
    bool initDepthNormalPipeline(VkDevice device, VkFormat normalFormat, VkFormat specularFormat, VkFormat depthFormat,
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
    // Camera-facing particle quads pulled from storage buffers: no vertex
    // input, depth tested without writing (the depth attachment is read-only),
    // premultiplied blending of the scene color without touching its alpha,
    // and a maximum blend of the reactive channel (blue) of the motion target.
    // Set 0 is the camera set, set 1 particleLayout; the vertex stage takes
    // vertexPushConstantSize bytes of push constants.
    bool initParticlePipeline(VkDevice device, VkFormat colorFormat, VkFormat motionFormat, VkFormat depthFormat,
                              VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout particleLayout,
                              VkShaderModule vertex, VkShaderModule fragment, VkPipelineCache pipelineCache,
                              uint32_t vertexPushConstantSize);
    // Picking id buffer (IdBufferPass): position and skin streams, both faces,
    // reversed-Z depth test and write, two unblended color targets (object id
    // and device depth). Set 0 is the camera set; vertex and fragment stages
    // share pushConstantSize bytes of push constants.
    bool initIdBufferPipeline(VkDevice device, VkFormat idFormat, VkFormat depthColorFormat, VkFormat depthFormat,
                              VkDescriptorSetLayout cameraLayout, VkShaderModule vertex, VkShaderModule fragment,
                              VkPipelineCache pipelineCache, uint32_t pushConstantSize);
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
