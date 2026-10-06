#pragma once

// GPU particles: emit, simulate, cull, sort, and draw.

#include "../Lighting.hpp"
#include "../Particles.hpp"
#include "../Rendering.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <vector>

// Everything one frame's particle simulation reads. Matrices and positions are
// world space; the view is right-handed with the camera looking down -z.
struct ParticleSimulationFrame {
    glm::mat4 view{1.0f};
    // The jittered projection the scene is drawn with.
    glm::mat4 projection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    // The frame's light-cluster grid (same projection) and uploaded light count.
    ClusterGrid clusters;
    uint32_t localLightCount{0};

    // Directional light, pointing toward the light, with radiance color * intensity.
    glm::vec3 lightDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 lightRadiance{0.0f};
    // Cascades of the directional shadow map, as in MeshPass's camera data;
    // zero samples the sun unshadowed.
    uint32_t shadowCascades{0};
    std::array<glm::mat4, 3> lightViewProjections{};
    glm::vec4 cascadeSplits{0.0f};

    // Ambient radiance without image-based lighting; with it, the irradiance
    // SH buffer scaled by ambientScale. Inside a bound irradiance volume its
    // probes replace either (fields mirror the camera data of the same name).
    glm::vec3 ambientRadiance{0.0f};
    bool environmentIrradiance{false};
    float ambientScale{1.0f};
    glm::vec4 irradianceVolumeMinimum{0.0f};
    glm::vec4 irradianceVolumeCellSize{1.0f};
    glm::uvec4 irradianceVolumeCounts{0u};

    // Directional shadow map array (DEPTH_READ_ONLY_OPTIMAL, writes visible to
    // compute) with its comparison sampler; local lights and cluster lists of
    // ClusteredLightingPass, the environment irradiance SH (vec4[9]), and the
    // irradiance volume probes, all with writes visible to compute.
    VkImageView shadowMap{VK_NULL_HANDLE};
    VkSampler shadowSampler{VK_NULL_HANDLE};
    VkBuffer localLights{VK_NULL_HANDLE};
    VkBuffer clusterRanges{VK_NULL_HANDLE};
    VkBuffer clusterLightIndices{VK_NULL_HANDLE};
    VkBuffer environmentIrradianceBuffer{VK_NULL_HANDLE};
    VkBuffer irradianceVolumeProbes{VK_NULL_HANDLE};
};

// The emitters to simulate this frame, in scene id order, with each one's
// sprite slot (0: procedural round sprite, otherwise an index into the draw's
// sprite views).
struct ParticleEmitterSet {
    std::span<const particles::EmitterSnapshot> emitters;
    std::span<const uint32_t> textureSlots;
    // Scene::contentGeneration of the scene the emitters came from; a change
    // restarts every emitter.
    uint64_t sceneGeneration{0};
};

// Where and how the particles of a simulated frame are drawn.
struct ParticleDrawTargets {
    VkImageView sceneColor{VK_NULL_HANDLE};
    VkImageView motion{VK_NULL_HANDLE};
    VkImage depthImage{VK_NULL_HANDLE};
    VkImageView depthView{VK_NULL_HANDLE};
    VkExtent2D renderExtent{};
    // MeshPass's camera set for this frame (view-projection, fog, cluster depth).
    VkDescriptorSet cameraSet{VK_NULL_HANDLE};
    glm::mat4 view{1.0f};
    // Sprite slots; slot 0 must still name a valid view (it is never sampled).
    std::array<VkImageView, particles::maximumSpriteTextures> spriteViews{};
    VkSampler spriteSampler{VK_NULL_HANDLE};
};

// GPU particle system: per frame, particle_emit.comp spawns new particles into
// their emitters' rings, particle_simulate.comp moves, culls, and lights live
// particles and appends visible ones to the sort keys and indirect draw
// arguments, particle_sort.comp orders the keys back to front, and
// particle.vert/particle.frag draw one premultiplied, soft, fogged quad per key.
//
// Contract for callers:
// - recordSimulation must be outside a rendering scope, after the frame's light
//   clusters and shadow maps, and before recordDraw of the same frame.
// - recordDraw must be outside a rendering scope with the scene depth in
//   DEPTH_ATTACHMENT_OPTIMAL and the color and motion targets in
//   COLOR_ATTACHMENT_OPTIMAL. It draws into them in its own rendering scope
//   (depth read-only, also sampled for soft particles) and returns with the
//   same layouts and its attachment writes visible to later attachment use.
//   It draws nothing unless this frame's recordSimulation recorded work, and
//   may run once per render view of the frame: the particles are simulated,
//   culled, and sorted once, for the frame's leading view.
//
// Synchronization: the pool, sort keys, and draw arguments persist across
// frames; each frame orders itself after earlier frames' compute, vertex, and
// indirect reads. Replaced buffers are released once the frames that used them
// completed.
class ParticlePass {
  public:
    static constexpr uint32_t pipelineCount() {
        // Emit, simulate, sort, and draw.
        return 4;
    }

    bool init(const VulkanContext& context, uint32_t frameCount, VkDescriptorSetLayout cameraLayout,
              VkFormat colorFormat, VkFormat motionFormat, VkFormat depthFormat);
    void shutdown();

    // Plans the frame (see particles::ParticleScheduler), sizes the pool for
    // quality, and records emission, simulation, and sorting. Returns false
    // only on failure; Off or an empty emitter set records nothing.
    bool recordSimulation(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint64_t frameSerial,
                          ParticleQuality quality, const ParticleEmitterSet& emitters,
                          const ParticleSimulationFrame& frame);

    // Particles advance by this fixed step per recorded frame instead of by
    // wall-clock time (see PostProcessPass::setFixedFrameSeconds); nullopt
    // restores wall-clock stepping.
    void setFixedFrameSeconds(std::optional<float> seconds) {
        m_fixedFrameSeconds = seconds;
    }

    // Whether this frame's recordSimulation recorded particles to draw.
    bool drawPending() const {
        return m_drawPending;
    }

    void recordDraw(VkCommandBuffer commandBuffer, const ParticleDrawTargets& targets);
    // Releases replaced buffers whose last frame has completed.
    void collect(uint64_t completedSerial);

    // Emitters and pool slots of the last recorded frame.
    uint32_t emitterCount() const {
        return m_emitterCount;
    }

    uint32_t capacity() const {
        return m_capacity;
    }

    // Emitters the last recorded frame left out (ParticleFramePlan::droppedEmitters).
    uint32_t droppedEmitters() const {
        return m_droppedEmitters;
    }

    uint64_t allocatedBytes() const;

  private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
        VkDeviceSize size{0};
    };

    // Persistent pool, sized for one quality's budget.
    struct Pool {
        Buffer particles;
        Buffer sortKeys;
        Buffer drawArguments;
        uint32_t budget{0};
        bool cleared{false};
    };

    struct FrameUploads {
        Buffer frame;
        Buffer emitters;
    };

    bool createLayouts();
    bool createPipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout cameraLayout, VkFormat colorFormat,
                         VkFormat motionFormat, VkFormat depthFormat);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& output);
    void destroyBuffer(Buffer& buffer);
    void destroyPool(Pool& pool);
    bool preparePool(uint32_t budget, uint64_t frameSerial);
    // Seconds to advance this frame, clamped so a hitch does not teleport particles.
    float stepSeconds();
    void uploadFrame(uint32_t frameIndex, const particles::ParticleFramePlan& plan, const ParticleEmitterSet& emitters,
                     const ParticleSimulationFrame& frame, uint32_t sortElements, float step);
    void pushSimulationDescriptors(VkCommandBuffer commandBuffer, VkPipelineLayout layout, uint32_t frameIndex,
                                   const ParticleSimulationFrame& frame);
    void recordSort(VkCommandBuffer commandBuffer, uint32_t sortElements);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_simulationLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_sortLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_drawLayout{VK_NULL_HANDLE};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    ComputePipeline m_emitPipeline;
    ComputePipeline m_simulatePipeline;
    ComputePipeline m_sortPipeline;
    GraphicsPipeline m_drawPipeline;
    std::vector<FrameUploads> m_uploads;
    Pool m_pool;
    particles::ParticleScheduler m_scheduler;
    DeferredReleaseQueue m_releaseQueue;
    std::optional<float> m_fixedFrameSeconds;
    std::optional<std::chrono::steady_clock::time_point> m_previousStepTime;
    bool m_drawPending{false};
    uint32_t m_emitterCount{0};
    uint32_t m_capacity{0};
    uint32_t m_droppedEmitters{0};
};
