#pragma once

// Point and spot light shadows rendered into the shared shadow atlas.

#include "../LocalShadows.hpp"
#include "../Rendering.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/FrameTimestamps.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <span>
#include <vector>

// Mirrors LocalShadowTile in shaders/include/local_shadow_tile.glsl
// (std430). One record per atlas tile, at the tile's index in
// LocalShadowCache::tiles().
struct GpuLocalShadowTile {
    // World to tile clip space of the depth the atlas holds (see LocalShadows.hpp).
    glm::mat4 viewProjection{1.0f};
    // Atlas uv of the tile's top-left corner (xy) and the tile's uv extent (zw).
    glm::vec4 atlasRect{0.0f};
    // x: world size of one shadow texel per unit distance from the light.
    glm::vec4 params{0.0f};
};

static_assert(sizeof(GpuLocalShadowTile) == 96, "GpuLocalShadowTile must match the std430 LocalShadowTile struct");

// What the renderer knows about the frame's local lights and casters.
struct LocalShadowFrameInput {
    LocalShadowQuality quality{LocalShadowQuality::Off};
    // The frame's uploaded local lights, in GpuLocalLight order.
    std::span<const LocalLight> lights;
    glm::vec3 cameraPosition{0.0f};
    // Casters that moved or animated since the previous frame.
    std::span<const ShadowCasterBounds> changedCasters;
    // Set when the caster set or materials changed, which stales every tile.
    bool invalidateAll{false};
};

// Owns the local-light shadow atlas, its comparison sampler, the per-frame
// tile records, and the depth-only pipeline that fills tiles.
//
// Camera-set contract: binding atlasBinding is a sampler2DShadow of the atlas
// (DEPTH_READ_ONLY_OPTIMAL, conventional depth, LESS_OR_EQUAL comparison) and
// binding tileBinding the frame slot's GpuLocalShadowTile array. A light's
// GpuLocalLight::spotCone.z names its first tile; point lights add the face.
//
// Per frame, record() must run before any command binds the frame slot's camera
// sets, because it may rebind the atlas (the atlas is reallocated when the
// quality level changes its size; the old image is released through the
// renderer's deferred-release queue). The atlas persists across frames:
// tiles whose light and casters did not change keep their depth, and only
// LocalShadowProfile::updateBudgetTiles tiles are re-rendered per frame.
class LocalShadowPass {
  public:
    static constexpr uint32_t atlasBinding = 14;
    static constexpr uint32_t tileBinding = 15;
    // Records per frame slot; the profiles allocate at most 144 tiles.
    static constexpr uint32_t maximumTiles = 256;

    // Draws the shadow casters of one tile. The pass has bound the pipeline,
    // the camera and material sets, the viewport, the depth bias, and the tile
    // index push constant. Returns false when the caller ran out of object
    // capacity; that light is then left unshadowed and retried next frame,
    // while the remaining scheduled lights are still drawn.
    using DrawCasters = std::function<bool(VkCommandBuffer, const glm::mat4& viewProjection)>;

    bool init(const VulkanContext& context, uint32_t frameCount, VkFormat depthFormat,
              VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout materialLayout);
    void shutdown();

    // Points both bindings of a frame slot's camera sets at this pass's
    // resources. init() callers use it for freshly allocated sets.
    void writeDescriptors(uint32_t frameIndex, std::span<const VkDescriptorSet> cameraSets);

    // Plans and records the frame's tile updates (between the begin and end
    // local-shadow timestamps when any tile is drawn), uploads the tile records
    // and returns, per input light, the first tile index or -1 when the light
    // is unshadowed. completionSerial is the frame serial after which a retired
    // atlas may be destroyed.
    std::vector<int32_t> record(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint64_t completionSerial,
                                const LocalShadowFrameInput& input, std::span<const VkDescriptorSet> cameraSets,
                                VkDescriptorSet materialSet, DeferredReleaseQueue& releases,
                                OptionalPassTimestamps& timestamps, const DrawCasters& drawCasters, RenderStats& stats);

    uint64_t atlasBytes() const;

  private:
    struct Atlas {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        uint32_t size{0};
    };

    struct FrameData {
        VkBuffer tiles{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
        // Atlas view the slot's camera sets currently reference.
        VkImageView boundAtlas{VK_NULL_HANDLE};
    };

    bool createAtlas(uint32_t size);
    void destroyAtlas(const Atlas& atlas) const;
    // Size the frame needs: the quality's atlas when lights request shadows,
    // otherwise the current atlas (a placeholder when Off or none exists).
    uint32_t requiredAtlasSize(LocalShadowQuality quality, bool shadowRequested) const;
    void renderTiles(VkCommandBuffer commandBuffer, std::span<const uint64_t> scheduled,
                     std::span<const VkDescriptorSet> cameraSets, VkDescriptorSet materialSet,
                     const DrawCasters& drawCasters, RenderStats& stats);
    void uploadTiles(uint32_t frameIndex);

    VkDevice m_device{VK_NULL_HANDLE};
    LogChannel m_log;
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkFormat m_depthFormat{VK_FORMAT_UNDEFINED};
    VkSampler m_sampler{VK_NULL_HANDLE};
    GraphicsPipeline m_pipeline;
    Atlas m_atlas;
    // The atlas left UNDEFINED once it was cleared and moved to DEPTH_READ_ONLY.
    bool m_atlasInitialized{false};
    std::vector<FrameData> m_frames;
    LocalShadowCache m_cache;
};
