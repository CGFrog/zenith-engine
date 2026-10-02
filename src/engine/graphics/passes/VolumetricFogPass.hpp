#pragma once

#include "../Rendering.hpp"
#include "../VolumetricFog.hpp"
#include "../pipeline/ComputePipeline.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <glm/glm.hpp>

#include <array>
#include <vector>

// Everything one frame's fog work reads. Matrices and positions are world
// space; the view is right-handed with the camera looking down -z.
struct VolumetricFogFrame {
    // The froxel grid, built from this frame's cluster grid (same projection).
    volumetric_fog::FroxelGrid grid;
    glm::mat4 inverseView{1.0f};
    // The previous frame's (jittered) view-projection, whose froxel layout the
    // history volume uses; ignored unless historyValid.
    glm::mat4 previousViewProjection{1.0f};
    // False after camera cuts, resizes, or anything else that breaks history.
    bool historyValid{false};
    glm::vec3 cameraPosition{0.0f};

    // Directional light, pointing toward the light, with radiance color * intensity.
    glm::vec3 lightDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 lightRadiance{0.0f};
    // Cascades of the directional shadow map, matching MeshPass's camera data;
    // zero samples the light unshadowed.
    uint32_t shadowCascades{0};
    std::array<glm::mat4, 3> lightViewProjections{};
    // Far distance of each cascade from the camera.
    glm::vec4 cascadeSplits{0.0f};

    // Ambient radiance arriving from every direction, used for isotropic
    // in-scattering. When environmentIrradiance is set, it instead comes from
    // the order-2 irradiance SH buffer (scaled by ambientScale).
    glm::vec3 ambientRadiance{0.0f};
    bool environmentIrradiance{false};
    float ambientScale{1.0f};

    VolumetricFogComponent medium;
    uint32_t localLightCount{0};

    // Directional shadow map array (DEPTH_READ_ONLY_OPTIMAL, writes visible to
    // compute) with a comparison sampler; local lights and cluster lists from
    // ClusteredLightingPass (visible to compute); the irradiance SH buffer of
    // EnvironmentPass (vec4 coefficients[9], visible to compute).
    VkImageView shadowMap{VK_NULL_HANDLE};
    VkSampler shadowSampler{VK_NULL_HANDLE};
    VkBuffer localLights{VK_NULL_HANDLE};
    VkBuffer clusterRanges{VK_NULL_HANDLE};
    VkBuffer clusterLightIndices{VK_NULL_HANDLE};
    VkBuffer environmentIrradianceBuffer{VK_NULL_HANDLE};
};

// Froxel volumetric fog: per frame, volumetric_fog_inject.comp evaluates the
// medium and its in-scattered sun (with cascade shadows), clustered local
// lights, and ambient light per froxel, blending it with the reprojected
// previous frame; volumetric_fog_integrate.comp then accumulates it front to
// back along depth.
//
// Contract for readers:
// - integratedView() is an RGBA16F 3D image in volumeLayout: rgb is the light
//   scattered toward the eye up to the end of each froxel slice, a the
//   transmittance (see volumetric_fog::FroxelGrid::integratedW for the w
//   lookup). Sample it with sampler() and composite color * a + rgb.
// - After record() it holds this frame's fog, visible to fragment shaders.
//   After recordCleared() (or before any record) it holds no fog (rgb 0, a 1).
// - The volumes are replaced when prepare() changes the quality; generation()
//   then changes and descriptors holding integratedView() must be rewritten.
//   Replaced volumes are released once the frames that used them completed.
//
// Synchronization: all volumes persist across frames and stay in GENERAL. The
// pass orders itself after earlier frames' fragment reads and fog writes.
class VolumetricFogPass {
  public:
    static constexpr VkFormat volumeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkImageLayout volumeLayout = VK_IMAGE_LAYOUT_GENERAL;

    static constexpr uint32_t pipelineCount() {
        return 2;
    }

    // Returns false only on failure; without RGBA16F 3D storage support the
    // pass still owns a cleared volume and supported() is false.
    bool init(const VulkanContext& context, uint32_t frameCount);
    void shutdown();

    bool supported() const {
        return m_supported;
    }

    // Sizes the volumes for quality (Off keeps a 1x1x1 cleared volume).
    // frameSerial identifies the frame being recorded; replaced volumes are
    // released once completedSerial reaches it (see collect).
    bool prepare(VolumetricFogQuality quality, uint64_t frameSerial);
    // Releases replaced volumes whose last frame has completed.
    void collect(uint64_t completedSerial);

    // Computes this frame's fog into the integrated volume. Must be outside a
    // rendering scope; frameIndex selects the frame slot's uniform buffer.
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const VolumetricFogFrame& frame);
    // Leaves the integrated volume holding no fog.
    void recordCleared(VkCommandBuffer commandBuffer);

    VkImageView integratedView() const {
        return m_volumes.integrated.view;
    }

    VkSampler sampler() const {
        return m_sampler;
    }

    uint64_t generation() const {
        return m_volumes.generation;
    }

    // Froxels per cluster of the current volumes; zero when Off.
    glm::uvec3 subdivision() const {
        return m_volumes.subdivision;
    }

    uint64_t allocatedBytes() const;

  private:
    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    enum class Contents {
        Undefined,
        Cleared,
        Fog,
    };

    struct Volumes {
        // Ping-pong scattering/extinction volumes: one is written each frame,
        // the other holds the previous frame's history.
        std::array<Image, 2> scattering;
        Image integrated;
        glm::uvec3 extent{1u};
        glm::uvec3 subdivision{0u};
        VolumetricFogQuality quality{VolumetricFogQuality::Off};
        uint64_t generation{0};
        Contents contents{Contents::Undefined};
        uint32_t current{0};
        // The scattering history describes the previous recorded frame.
        bool historyValid{false};
        glm::vec2 previousSliceScaleBias{0.0f};
    };

    struct UniformBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
    };

    bool createLayouts();
    bool createPipelines(VkPipelineCache pipelineCache);
    bool createUniformBuffers(uint32_t frameCount);
    bool createVolumes(VolumetricFogQuality quality, Volumes& output);
    bool createImage(const glm::uvec3& extent, VkImageUsageFlags usage, Image& output);
    void destroyImage(Image& image);
    void destroyVolumes(Volumes& volumes);
    void initializeVolumes(VkCommandBuffer commandBuffer);
    void recordInject(VkCommandBuffer commandBuffer, uint32_t frameIndex, const VolumetricFogFrame& frame);
    void recordIntegrate(VkCommandBuffer commandBuffer, const VolumetricFogFrame& frame);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    bool m_supported{false};
    VkDescriptorSetLayout m_injectLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_integrateLayout{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    ComputePipeline m_injectPipeline;
    ComputePipeline m_integratePipeline;
    std::vector<UniformBuffer> m_uniforms;
    Volumes m_volumes;
    uint64_t m_nextGeneration{1};
    uint32_t m_frameNumber{0};
    DeferredReleaseQueue m_releaseQueue;
};
