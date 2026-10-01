#pragma once

#include "engine/assets/Assets.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/graphics/pipeline/ComputePipeline.hpp"
#include "engine/graphics/pipeline/GraphicsPipeline.hpp"
#include "engine/graphics/vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <array>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Radiance source of one environment: an equirectangular map when one is
// loaded, otherwise the analytic sky built from the two colors.
struct EnvironmentSource {
    TextureHandle map{};
    const TextureAsset* mapAsset{nullptr};
    glm::vec3 skyColor{0.0f};
    glm::vec3 groundColor{0.0f};
};

struct EnvironmentFrameRequest {
    EnvironmentSource primary;
    // Dominant reflection probe, blended over the primary environment by weight.
    std::optional<EnvironmentSource> probe;
    float probeWeight{0.0f};
    EnvironmentQuality quality{EnvironmentQuality::ImageBasedHigh};
};

// What shading needs alongside the environment maps bound for a frame.
struct EnvironmentFrameState {
    float probeWeight{0.0f};
    // Mip of the prefiltered specular cube that holds roughness 1.
    float specularMaximumLod{0.0f};
};

// Split-sum image-based lighting. For each environment source it precomputes on
// the GPU, once and cached by source: a radiance cube with mips, a GGX-prefiltered
// specular cube, and order-2 spherical-harmonic irradiance; a BRDF table is
// computed once. Precomputation is recorded into the frame's command buffer
// before the scene pass, and retired resources wait for their frames to finish.
class EnvironmentPass {
  public:
    // Camera-set bindings this pass owns; mirrored in shaders/mesh.frag.
    static constexpr uint32_t specularBinding = 5;
    static constexpr uint32_t irradianceBinding = 6;
    static constexpr uint32_t brdfBinding = 7;
    // Slot 0 is the primary environment, slot 1 the dominant reflection probe.
    static constexpr uint32_t environmentSlots = 2;

    bool init(const VulkanContext& context, VkDescriptorSetLayout cameraLayout, VkFormat colorFormat,
              VkFormat motionFormat, VkFormat depthFormat, uint32_t frameCount);
    // Records missing precomputation into commandBuffer, which must be outside a
    // rendering scope, and points the frame's camera sets at the requested
    // environments. Call before the camera sets are bound this frame;
    // frameSerial increases by one per recorded frame. Built environments count
    // as ready once recorded, so the command buffer must be submitted: the
    // renderer ends its frame loop when recording fails. A map that cannot be
    // built falls back to the analytic sky.
    std::optional<EnvironmentFrameState> prepare(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                                 uint64_t frameSerial, const EnvironmentFrameRequest& request,
                                                 std::span<const VkDescriptorSet> cameraSets);
    // Draws the primary environment where the scene pass left far-plane depth,
    // scaled like its specular reflections so mirrors match the background.
    // temporalJitter holds the current (xy) and previous (zw) NDC projection
    // jitter, which motion vectors exclude.
    void recordSkybox(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkDescriptorSet cameraSet,
                      float radianceScale, const glm::vec4& temporalJitter) const;
    void shutdown();

  private:
    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    struct Environment {
        Image radiance;
        Image specular;
        Buffer irradiance;
        bool procedural{true};
        uint64_t lastUsedSerial{0};
        // Never reused, unlike Vulkan handles of destroyed environments.
        uint64_t id{0};
    };

    // Environments last written into one frame in flight's descriptor sets, by
    // id; zero means none.
    struct FrameBinding {
        std::vector<VkDescriptorSet> cameraSets;
        std::array<uint64_t, environmentSlots> environments{};
        uint64_t sky{0};
        bool proceduralSky{true};
    };

    struct Resolution {
        uint32_t radianceSize{0};
        uint32_t specularSize{0};
        uint32_t specularMips{0};
        uint32_t sampleCount{0};
    };

    static Resolution resolutionFor(EnvironmentQuality quality, bool usesMap);
    bool createLayouts();
    bool createPipelines(const VulkanContext& context, VkDescriptorSetLayout cameraLayout, VkFormat colorFormat,
                         VkFormat motionFormat, VkFormat depthFormat);
    bool createSamplers();
    bool createSkySets(uint32_t frameCount);
    bool buildBrdfTable(VkCommandBuffer commandBuffer);
    const Environment* ensureEnvironment(VkCommandBuffer commandBuffer, const EnvironmentSource& source,
                                         EnvironmentQuality quality);
    bool buildEnvironment(VkCommandBuffer commandBuffer, const EnvironmentSource& source, const Resolution& resolution,
                          Environment& environment);
    bool renderRadiance(VkCommandBuffer commandBuffer, const EnvironmentSource& source, const Resolution& resolution,
                        const Image& radiance);
    bool uploadEquirectangular(VkCommandBuffer commandBuffer, const TextureAsset& texture, Image& output,
                               uint32_t& mipCount);
    bool prefilterSpecular(VkCommandBuffer commandBuffer, const Resolution& resolution, const Environment& environment);
    bool projectIrradiance(VkCommandBuffer commandBuffer, const Resolution& resolution, const Environment& environment);
    VkDescriptorSet allocateComputeSet();
    void writeCameraSets(FrameBinding& binding, std::span<const VkDescriptorSet> cameraSets,
                         const std::array<const Environment*, environmentSlots>& environments);
    void evictUnusedEnvironments();
    bool createImage(const VkImageCreateInfo& info, Image& output, const char* operation);
    VkImageView createView(VkImage image, VkImageViewType type, VkFormat format, uint32_t baseMip, uint32_t mipCount,
                           uint32_t layerCount);
    void retireImage(const Image& image);
    void retireBuffer(const Buffer& buffer);
    void retireView(VkImageView view);
    void retireComputeSet(VkDescriptorSet set);
    void destroyEnvironment(const Environment& environment);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    uint32_t m_frameCount{0};
    uint64_t m_frameSerial{0};
    DeferredReleaseQueue m_releaseQueue;
    VkDescriptorSetLayout m_computeLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_skyLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_computePool{VK_NULL_HANDLE};
    VkDescriptorPool m_skyPool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_skySets;
    VkSampler m_cubeSampler{VK_NULL_HANDLE};
    VkSampler m_equirectangularSampler{VK_NULL_HANDLE};
    ComputePipeline m_equirectangularPipeline;
    ComputePipeline m_proceduralPipeline;
    ComputePipeline m_prefilterPipeline;
    ComputePipeline m_irradiancePipeline;
    ComputePipeline m_brdfPipeline;
    GraphicsPipeline m_skyboxPipeline;
    Image m_brdfTable;
    bool m_brdfReady{false};
    std::unordered_map<uint64_t, Environment> m_environments;
    std::unordered_set<uint64_t> m_failedEnvironments;
    uint64_t m_nextEnvironmentId{1};
    std::vector<FrameBinding> m_frameBindings;
};
