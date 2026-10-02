#pragma once

#include "engine/graphics/Rendering.hpp"
#include "engine/graphics/ScreenSpaceReflectionTrace.hpp"
#include "engine/graphics/passes/SceneGeometryViews.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>

// Camera state the trace needs; the projection is the one that rendered the
// frame's depth, including any TAA jitter.
struct ScreenSpaceReflectionCamera {
    glm::mat4 projection{1.0f};
    // Varies the rough-surface ray jitter and the half-resolution footprint.
    uint32_t frameNumber{0};
    // True when the temporal resolve accumulates the result, which allows
    // stochastic ray directions.
    bool temporalAccumulation{false};
};

// Hi-Z screen-space reflections (shaders/ssr_trace.comp and
// shaders/ssr_composite.comp), blended into the lit HDR scene after opaque
// surfaces and the background and before blended surfaces, so the temporal
// resolve accumulates them. Pixels without a confident hit keep the scene's
// IBL and probe specular unchanged. The composite assumes dielectric
// reflectance (F0 = 0.04) because the prepass stores no specular color, so
// metals get weak, untinted screen-space reflections over their IBL specular.
//
// The trace reads the depth prepass normals and the depth pyramid
// (SceneGeometryViews) and samples the current frame's HDR scene at the hit,
// so reflections show this frame's direct and indirect lighting (one bounce;
// reflections do not contain earlier reflections). It writes premultiplied
// radiance and confidence into one reflection target at half or full render
// resolution; the composite upsamples it and rewrites the scene in place.
//
// The reflection target is shared by consecutive frames like the temporal
// history: it is allocated on first use, stays in GENERAL, and each record()
// orders its writes after the previous frame's reads. It is sized for full
// resolution at the output extent, so switching quality never reallocates an
// image that frames in flight may still read; it is released on resize and
// shutdown.
class ScreenSpaceReflectionPass {
  public:
    static constexpr VkFormat targetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    bool init(const VulkanContext& context, VkExtent2D outputExtent);
    // Releases the target; the caller must ensure the GPU no longer uses it.
    void resize(VkExtent2D outputExtent);
    void shutdown();

    // Traces and composites into sceneView. The scene image must be in
    // GENERAL with its writes visible to compute sampled and storage access;
    // on return the composite's storage writes still need a barrier before
    // later readers. quality must not be ProbesOnly. Fails only if the target
    // cannot be allocated.
    bool record(VkCommandBuffer commandBuffer, VkImageView sceneView, const SceneGeometryViews& geometry,
                ReflectionQuality quality, const ScreenSpaceReflectionCamera& camera);

    uint64_t targetBytes() const;

  private:
    bool createLayout();
    bool ensureTarget();
    void destroyTarget();
    VkExtent2D tracedExtent(VkExtent2D renderExtent, ReflectionQuality quality) const;
    void recordTrace(VkCommandBuffer commandBuffer, VkImageView sceneView, const SceneGeometryViews& geometry,
                     VkExtent2D traced, ReflectionQuality quality, const ScreenSpaceReflectionCamera& camera) const;
    void recordComposite(VkCommandBuffer commandBuffer, VkImageView sceneView, const SceneGeometryViews& geometry,
                         VkExtent2D traced, ReflectionQuality quality, const ScreenSpaceReflectionCamera& camera) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_tracePipeline;
    ComputePipeline m_compositePipeline;
    VkImage m_target{VK_NULL_HANDLE};
    VkImageView m_targetView{VK_NULL_HANDLE};
    VmaAllocation m_targetAllocation{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    screen_space_reflection::TraceSettings m_traceSettings;
    screen_space_reflection::CompositeSettings m_compositeSettings;
};
