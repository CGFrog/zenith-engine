#pragma once

// Renders object ids and depth for GPU picking.

#include "../RenderPicking.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/IdReadback.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <volk.h>

// Push constants of one id-buffer draw; mirrors IdDraw in shaders/id_buffer.*.
struct IdBufferDraw {
    glm::mat4 modelViewProjection{1.0f};
    // The object's skin palette range in the camera set's palette buffer;
    // jointCount 0 draws the bind pose.
    uint32_t firstJoint{0};
    uint32_t jointCount{0};
    // Written to the id target; 0 is reserved for "no object".
    uint32_t objectId{0};
    uint32_t unused{0};
};

// Draws object ids and device depth for one pick region of a render view and
// copies them into an IdReadback. Owns its pipeline and its targets (an
// R32_UINT id image, an R32_SFLOAT depth image, and a depth attachment), which
// grow to the largest region requested so far. The caller records nothing at
// all on frames without a pick request, so the pass costs nothing then.
//
// Per region: begin -> draw (once per object surface, with the caller binding
// the geometry streams) -> end. Regions address the view's output pixels; the
// viewport is offset so only the region rasterizes, into the targets' top-left.
class IdBufferPass {
  public:
    static constexpr VkFormat idFormat = VK_FORMAT_R32_UINT;
    static constexpr VkFormat depthColorFormat = VK_FORMAT_R32_SFLOAT;

    IdBufferPass() = default;
    IdBufferPass(const IdBufferPass&) = delete;
    IdBufferPass& operator=(const IdBufferPass&) = delete;

    // cameraLayout: the mesh camera set layout, whose binding 3 holds the skin
    // palettes. depthFormat: a depth-attachment format.
    bool init(const VulkanContext& context, VkPipelineCache pipelineCache, VkDescriptorSetLayout cameraLayout,
              VkFormat depthFormat);
    // Clears the targets for `region` (already clamped to viewExtent) and binds
    // the pipeline with `cameraSet`. Targets replaced to fit the region are
    // released once frame `frameSerial` completed (see collect).
    bool begin(VkCommandBuffer commandBuffer, uint64_t frameSerial, VkExtent2D viewExtent, const PickRect& region,
               VkDescriptorSet cameraSet);
    // Draws one indexed surface; the caller bound its vertex and index buffers.
    void draw(VkCommandBuffer commandBuffer, const IdBufferDraw& constants, uint32_t indexCount, uint32_t firstIndex,
              int32_t vertexOffset) const;
    // Ends the region and records its copy into `readback`, which was
    // prepared for the region's size.
    void end(VkCommandBuffer commandBuffer, const IdReadback& readback) const;
    // Releases targets retired by frames up to completedSerial.
    void collect(uint64_t completedSerial);
    void shutdown();

    static constexpr uint32_t pipelineCount() {
        return 1;
    }

  private:
    struct Targets {
        VkImage idImage{VK_NULL_HANDLE};
        VmaAllocation idAllocation{VK_NULL_HANDLE};
        VkImageView idView{VK_NULL_HANDLE};
        VkImage depthColorImage{VK_NULL_HANDLE};
        VmaAllocation depthColorAllocation{VK_NULL_HANDLE};
        VkImageView depthColorView{VK_NULL_HANDLE};
        VkImage depthImage{VK_NULL_HANDLE};
        VmaAllocation depthAllocation{VK_NULL_HANDLE};
        VkImageView depthView{VK_NULL_HANDLE};
        VkExtent2D extent{};
    };

    bool ensureTargets(VkExtent2D extent, uint64_t frameSerial);
    bool createTargets(VkExtent2D extent, Targets& targets) const;
    bool createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkExtent2D extent,
                     VkImage& image, VmaAllocation& allocation, VkImageView& view) const;
    void destroyTargets(const Targets& targets) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkFormat m_depthFormat{VK_FORMAT_UNDEFINED};
    GraphicsPipeline m_pipeline;
    Targets m_targets;
    DeferredReleaseQueue m_releaseQueue;
};
