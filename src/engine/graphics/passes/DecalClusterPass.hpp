#pragma once

#include "engine/graphics/Decals.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"

#include <array>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <vector>

// Mirrors DecalRecord in shaders/mesh.frag and shaders/decal_clusters.comp (std430).
struct GpuDecal {
    // Rows of the affine world-to-decal transform (DecalProjector::worldToDecal).
    std::array<glm::vec4, 3> worldToDecal{};
    glm::vec4 tintOpacity{1.0f};
    // x: roughness, y: roughness strength, z: metallic, w: metallic strength.
    glm::vec4 surface{0.0f};
    // x: base color strength, y: normal strength, z, w: angle fade scale, bias.
    glm::vec4 blend{0.0f};
    // x: albedo bindless slot (sRGB view), y: normal slot (linear view),
    // z: flags (decalAlbedoFlag, decalNormalFlag); w unused.
    glm::uvec4 textures{0u};
    // View-space bounds of the box, read by decal_clusters.comp.
    glm::vec4 viewMinimum{0.0f};
    glm::vec4 viewMaximum{0.0f};
};

static_assert(sizeof(GpuDecal) == 144);

// Flags in GpuDecal::textures.z; mirror DECAL_HAS_ALBEDO and DECAL_HAS_NORMAL.
inline constexpr uint32_t decalAlbedoFlag = 1u;
inline constexpr uint32_t decalNormalFlag = 2u;

// Bindless slots of a decal's textures; an empty slot leaves the texture out.
struct DecalTextureSlots {
    std::optional<uint32_t> albedo;
    std::optional<uint32_t> normal;
};

GpuDecal packDecal(const DecalProjector& decal, const DecalTextureSlots& slots, const glm::mat4& view);

// Statistics a frame's assignment produced, read once it retired.
struct DecalClusterStatistics {
    uint32_t maximumDecalsInCluster{0};
    uint32_t discardedAssignments{0};
};

// Owns each frame's decal records (camera binding recordsBinding, laid out as
// Decals in shaders/mesh.frag: a header whose x is the decal count, then the
// records) and per-cluster decal lists (clusterListsBinding: per cluster one
// count and maximumDecalsPerCluster indices) written by decal_clusters.comp.
// Per frame: beginFrame (host, every frame, so a zero count disables decals)
// -> record (before the scene pass) -> mesh.frag reads both buffers. Every
// frame in flight owns its buffers, rewritten once the slot's previous
// submission retired.
class DecalClusterPass {
  public:
    static constexpr uint32_t recordsBinding = 19;
    static constexpr uint32_t clusterListsBinding = 20;

    struct FrameBuffers {
        VkBuffer records{VK_NULL_HANDLE};
        VkBuffer clusterLists{VK_NULL_HANDLE};
    };

    bool init(const VulkanContext& context, uint32_t frameCount, const ClusterGridConfiguration& configuration);
    void shutdown();

    // Reads the slot's retired statistics and uploads up to maximumDecals
    // records in application order. Returns the number uploaded.
    uint32_t beginFrame(uint32_t frameIndex, std::span<const GpuDecal> decals, DecalClusterStatistics& retired);
    // Assigns the uploaded decals to clusters and makes the lists visible to
    // fragment shaders. Records nothing when no decal was uploaded. Must be
    // outside a rendering scope; the grid must come from makeClusterGrid with
    // the configuration given to init.
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const ClusterGrid& grid);

    FrameBuffers frameBuffers(uint32_t frameIndex) const;

    // Device memory of every frame's buffers.
    uint64_t allocatedBytes() const;

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
        VkDeviceSize size{0};
    };

    struct FrameData {
        Buffer records;
        Buffer clusterLists;
        Buffer statistics;
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        uint32_t decalCount{0};
        bool statisticsPending{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createFrameBuffers(FrameData& frame);
    bool createBuffer(VkDeviceSize size, BufferAccess access, Buffer& output);
    void destroyBuffer(Buffer& buffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    ClusterGridConfiguration m_configuration;
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    ComputePipeline m_pipeline;
    std::vector<FrameData> m_frames;
};
