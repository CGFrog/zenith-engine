#pragma once

// Assigns point and spot lights to the cluster grid (light_clusters.comp).

#include "../Lighting.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <glm/glm.hpp>
#include <span>
#include <vector>

// Mirrors LocalLight in shaders/include/local_light.glsl (std430).
struct GpuLocalLight {
    glm::vec4 positionRange{0.0f};  // world position, range
    glm::vec4 colorIntensity{0.0f}; // linear color, intensity
    glm::vec4 directionType{0.0f};  // world direction, type (0 point, 1 spot)
    // x, y: inner and outer cone cosines (spot lights). z: index of the
    // light's first record in the local shadow tile buffer (camera binding 15,
    // see LocalShadowPass), or -1 when the light is unshadowed this frame.
    glm::vec4 spotCone{0.0f};
};

static_assert(sizeof(GpuLocalLight) == 64, "GpuLocalLight must match the std430 LocalLight struct");

// Cluster statistics a frame's assignment produced, read once it retired.
struct ClusterLightStatistics {
    uint32_t maximumLightsInCluster{0};
    uint32_t discardedAssignments{0};
};

// Owns the per-frame local-light list and the cluster light lists that
// light_clusters.comp builds from it. Per frame: beginFrame (host) -> record
// (before the scene pass) -> shading reads the buffers in fragment shaders.
// Every frame in flight has its own buffers, so the host may rewrite a slot
// once that slot's previous submission retired.
class ClusteredLightingPass {
  public:
    struct FrameBuffers {
        VkBuffer lights{VK_NULL_HANDLE};
        VkBuffer clusterRanges{VK_NULL_HANDLE};
        VkBuffer lightIndices{VK_NULL_HANDLE};
    };

    bool init(const VulkanContext& context, uint32_t frameCount, const ClusterGridConfiguration& configuration,
              uint32_t lightCapacity);
    void shutdown();

    // Reads the slot's retired statistics, resets them, and uploads up to
    // lightCapacity() world-space lights. Returns the number uploaded.
    uint32_t beginFrame(uint32_t frameIndex, std::span<const LocalLight> lights, ClusterLightStatistics& retired);
    // Sets each uploaded light's shadow tile (GpuLocalLight::spotCone.z) from
    // the entry with the same index; negative entries mean unshadowed. Lights
    // start unshadowed in beginFrame. Must precede the frame's submission.
    void assignShadowTiles(uint32_t frameIndex, std::span<const int32_t> firstShadowTiles);
    // Assigns the uploaded lights to clusters and makes the lists visible to
    // fragment shaders. Records nothing when no lights were uploaded. The grid
    // must come from makeClusterGrid with configuration().
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const ClusterGrid& grid, const glm::mat4& view);

    FrameBuffers frameBuffers(uint32_t frameIndex) const;

    const ClusterGridConfiguration& configuration() const {
        return m_configuration;
    }

    uint32_t lightCapacity() const {
        return m_lightCapacity;
    }

  private:
    enum class BufferAccess {
        Device,
        HostWrite,
        HostReadback,
    };

    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
    };

    struct FrameData {
        Buffer lights;
        Buffer clusterRanges;
        Buffer lightIndices;
        Buffer statistics;
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        uint32_t lightCount{0};
        bool statisticsPending{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createFrameBuffers(FrameData& frame);
    bool createBuffer(VkDeviceSize size, BufferAccess access, Buffer& output);
    void destroyBuffer(Buffer& buffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    ClusterGridConfiguration m_configuration;
    uint32_t m_lightCapacity{0};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    std::vector<FrameData> m_frames;
};
