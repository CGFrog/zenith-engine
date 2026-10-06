#pragma once

// Assigns reflection probes to clusters for per-pixel blending.

#include "engine/graphics/ReflectionProbes.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>
#include <span>
#include <vector>

// Mirrors ReflectionProbeRecord in shaders/mesh.frag and
// shaders/reflection_probe_clusters.comp (std430).
struct GpuReflectionProbe {
    glm::vec4 centerBlend{0.0f}; // world box center, blend distance
    glm::vec4 halfExtents{0.0f}; // world box half extents; w unused
    glm::ivec4 parameters{0};    // x: priority, y: probe cache slot
    // View-space bounds of the influence volume (see reflectionProbeViewBounds).
    glm::vec4 viewMinimum{0.0f};
    glm::vec4 viewMaximum{0.0f};
};

static_assert(sizeof(GpuReflectionProbe) == 80);

GpuReflectionProbe packReflectionProbe(const ReflectionProbeVolume& probe, uint32_t cacheSlot, const glm::mat4& view);

// Owns each frame's probe buffer, bound at camera-set binding 17 and laid out
// as ReflectionProbes in shaders/mesh.frag: a header (x: probe count), the
// probe records, nine irradiance coefficients per record gathered from the
// resident probe cache, and one probe mask per cluster written by
// reflection_probe_clusters.comp. Every frame in flight owns its buffer, and
// all of it is written by GPU commands recorded in that frame.
class ReflectionProbeClusterPass {
  public:
    static constexpr uint32_t cameraBinding = 17;

    bool init(const VulkanContext& context, uint32_t frameCount, const ClusterGridConfiguration& configuration);
    void shutdown();

    // Uploads probes (at most maximumResidentReflectionProbes; the record index
    // is the per-pixel tie-break, so pass them in entity order), copies each
    // record's coefficients from irradianceSource (nine vec4 per cache slot,
    // made visible to transfers by EnvironmentPass), assigns the probes to
    // clusters, and makes the buffer visible to fragment shaders. Records
    // nothing for an empty span. Must be outside a rendering scope; the grid
    // must come from makeClusterGrid with the configuration given to init.
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, std::span<const GpuReflectionProbe> probes,
                VkBuffer irradianceSource, const ClusterGrid& grid);

    VkBuffer frameBuffer(uint32_t frameIndex) const {
        return m_frames[frameIndex].buffer;
    }

  private:
    struct FrameData {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    bool createFrames(uint32_t frameCount);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    ClusterGridConfiguration m_configuration;
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    std::vector<FrameData> m_frames;
};
