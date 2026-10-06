#pragma once

// Draws DebugDraw lines and shapes into each view.

#include "../DebugDraw.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <span>
#include <vector>

// One view's debug lines for DebugDrawPass::record.
struct DebugDrawPassView {
    // Post-processed output in COLOR_ATTACHMENT_OPTIMAL, written by the
    // view's last post-processing pass.
    VkImageView colorView{VK_NULL_HANDLE};
    VkExtent2D extent{};
    // Scene depth in DEPTH_READ_ONLY_OPTIMAL, already made visible to fragment
    // shader reads; valid within renderExtent from its top-left corner.
    VkImageView depthView{VK_NULL_HANDLE};
    VkExtent2D renderExtent{};
    // Unjittered view-projection (reversed Z) and the planes it was built with.
    glm::mat4 viewProjection{1.0f};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
    // The view's own primitives, drawn after the shared ones; may be null.
    const DebugDraw* lines{nullptr};
};

// Draws DebugDraw line lists (shaders/debug_line.*) into post-processed view
// outputs as LINE_LIST primitives, after tone mapping, colour grading and
// upscaling and before the UI. Drawing in the display-referred output keeps
// colours exact and lines free of TAA history smear: lines use the unjittered
// projection and are never accumulated. Depth-tested lines compare their
// distance with the view's scene depth in the fragment shader (the depth
// buffer may be at render resolution while lines draw at output resolution).
//
// Owns one host-visible vertex buffer per frame in flight, grown on demand
// and reused otherwise, and per frame slot one depth descriptor set per view.
// record() requires that the frame slot's previous submission retired.
class DebugDrawPass {
  public:
    bool init(const VulkanContext& context, VkFormat colorFormat, uint32_t frameCount);
    void shutdown();

    // Uploads `shared` once and the views' own lines, then draws the shared
    // and own lines into every view. Records nothing without views. Fails
    // when the vertex buffer cannot grow.
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const DebugDraw& shared,
                std::span<const DebugDrawPassView> views);

    // Device memory of the vertex buffers.
    uint64_t allocatedBytes() const;

  private:
    struct VertexBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
        VkDeviceSize capacity{0};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createPipeline(VkFormat colorFormat);
    bool reserveVertices(VertexBuffer& buffer, VkDeviceSize bytes);
    void destroy(VertexBuffer& buffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    LogChannel m_log;
    bool m_linearizeColors{false};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    // [frame slot][view index within the frame].
    std::vector<std::vector<VkDescriptorSet>> m_depthSets;
    std::vector<VertexBuffer> m_vertexBuffers;
};
