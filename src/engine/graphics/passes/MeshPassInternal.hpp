#pragma once

// Private to the MeshPass*.cpp translation units: constants, GPU record
// layouts, and helpers more than one of them shares. Not part of the
// published engine API.

#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"
#include "MeshPass.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mesh_pass_detail {
constexpr uint32_t shadowResolution = 1448;
constexpr uint32_t maximumShadowCascades = 3;
constexpr uint32_t maximumObjectsPerFrame = 16384;
constexpr uint32_t maximumSkinMatricesPerFrame = 65536;
constexpr uint32_t maximumMaterials = 4096;
constexpr uint32_t maximumLocalLights = 4096;

// Every GPU-driven object reserves an instance slot in each of its LOD batches.
constexpr uint32_t maximumGpuDrivenInstances = maximumObjectsPerFrame * maximumMeshLods;
// Draw groups are (pipeline bucket, geometry page) pairs.
constexpr uint32_t maximumGpuDrawGroups = 64;
// Pipeline buckets used as IndirectDrawKey::pipeline.
constexpr uint32_t singleSidedBucket = 0;
constexpr uint32_t doubleSidedBucket = 1;
// Per-frame object buffer layout: one region per shadow cascade, then the
// region for objects the CPU submits to the main view, then the region every
// local-light shadow tile drawn this frame shares.
constexpr uint32_t mainObjectBase = maximumShadowCascades * maximumObjectsPerFrame;
constexpr uint32_t localShadowObjectBase = mainObjectBase + maximumObjectsPerFrame;
constexpr uint32_t objectRegionCount = maximumShadowCascades + 2;
// Camera-set binding of the previous frame's skin palettes (mesh.vert).
constexpr uint32_t previousSkinPaletteBinding = 12;
// Camera-set binding of the frame's screen-space ambient occlusion (mesh.frag).
constexpr uint32_t ambientOcclusionBinding = 13;
// Camera-set binding of the integrated volumetric fog (mesh.frag, skybox.frag).
constexpr uint32_t volumetricFogBinding = 18;
// Camera-set binding of the transmission background (mesh.frag).
constexpr uint32_t transmissionBackgroundBinding = 24;

struct CameraData {
    glm::mat4 viewProjection{1.0f};
    glm::mat4 previousViewProjection{1.0f};
    std::array<glm::mat4, maximumShadowCascades> lightViewProjections{};
    glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 lightDirection{0.4f, 0.8f, 0.5f, 5.0f};
    glm::vec4 lightColor{1.0f, 0.91f, 0.78f, 1.0f};
    glm::vec4 environmentSky{0.20f, 0.24f, 0.34f, 0.6f};
    glm::vec4 environmentGround{0.08f, 0.065f, 0.05f, 0.35f};
    glm::vec4 shadowSettings{0.01f, 1.0f, 0.0f, 0.0f};
    glm::vec4 cascadeSplits{1000.0f};
    glm::vec4 renderingSettings{0.0f};
    std::array<glm::vec4, 6> environmentSpecular{};
    // Clustered local lighting (see ClusterGrid in Lighting.hpp). x, y: slice
    // log scale and bias; z, w: view depth = z / (fragment depth + w).
    glm::vec4 clusterDepth{0.0f};
    // xy: cluster columns and rows per render-extent pixel.
    glm::vec4 clusterTileScale{0.0f};
    // Columns, rows, depth slices, uploaded local-light count.
    glm::uvec4 clusterGrid{0u};
    // x: image-based lighting enabled, y: reflection-probe blend weight,
    // z: prefiltered specular mip holding roughness 1, w: ambient-occlusion
    // texels per render pixel (see ambient_occlusion::occlusionUv).
    glm::vec4 environmentParams{0.0f};
    // Reflection probes (U9): x: 1 when per-pixel probe blending is active,
    // y: resident probe count.
    glm::vec4 reflectionProbeParams{0.0f};
    // Volumetric fog (U10); see VolumetricFog.hpp. x: 1 composites the
    // integrated fog volume; y, z: froxel slice = log(view depth) * y + z;
    // w: froxel depth slices.
    glm::vec4 fogVolume{0.0f};
    // xy: 1 / internal render extent, the uv per render pixel of the fog volume
    // and the transmission background.
    glm::vec4 fogScreen{0.0f};
    // Irradiance volume (U6); see irradiance_volume in IrradianceVolume.hpp.
    // xyz: world-space minimum corner of the baked box, w: intensity.
    glm::vec4 irradianceVolumeMinimum{0.0f};
    // xyz: world-space probe spacing, w: edge blend distance.
    glm::vec4 irradianceVolumeCellSize{1.0f};
    // xyz: probes per axis, w: 1 when a volume is bound, else 0.
    glm::uvec4 irradianceVolumeCounts{0u};
    // x: normal bias, y: view bias (world units); zw unused.
    glm::vec4 irradianceVolumeBias{0.0f};
    // Temporal resolve: NDC jitter of the current (xy) and previous (zw)
    // projections, removed from motion vectors in mesh.frag. Keep last.
    glm::vec4 temporalJitter{0.0f};
};

// Mirrors RenderObject in the mesh, shadow, outline, and selection shaders.
struct ObjectData {
    glm::mat4 model{1.0f};
    glm::mat4 previousModel{1.0f};
    // x: skin palette offset, y: joint count, z: material record index.
    glm::uvec4 skin{0};
};

inline void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect,
                         VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                         uint32_t layerCount = 1) {
    VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = layerCount}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

inline uint64_t handleKey(MeshHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

inline uint64_t handleKey(MaterialHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

inline uint64_t handleKey(TextureHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

inline uint64_t handleKey(GpuSceneHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

template <typename Handle> Handle handleFromKey(uint64_t key) {
    return {static_cast<uint32_t>(key), static_cast<uint32_t>(key >> 32)};
}

// Bindings of the camera set (set 0 of the mesh, shadow, outline, particle,
// and skybox pipelines). Every view's camera set uses this layout.
inline std::vector<VkDescriptorSetLayoutBinding> cameraLayoutBindings() {
    return {{.binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = 2,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
            {.binding = 3,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
            {.binding = 4,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Image-based lighting, written by EnvironmentPass.
            {.binding = EnvironmentPass::specularBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = EnvironmentPass::environmentSlots,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = EnvironmentPass::irradianceBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = EnvironmentPass::environmentSlots,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = EnvironmentPass::brdfBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Irradiance volume probes, written by IrradianceVolumePass.
            {.binding = IrradianceVolumePass::binding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Clustered local lighting: lights, per-cluster ranges, light indices.
            {.binding = 9,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = 10,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = 11,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = previousSkinPaletteBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
            // Screen-space ambient occlusion, written by AmbientOcclusionPass.
            {.binding = ambientOcclusionBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Local-light shadow atlas and tile records, written by LocalShadowPass.
            {.binding = LocalShadowPass::atlasBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = LocalShadowPass::tileBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
            // Per-pixel reflection probes: the resident probe cube array, written
            // by EnvironmentPass, and the frame's probe buffer.
            {.binding = EnvironmentPass::probeSpecularBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = ReflectionProbeClusterPass::cameraBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Integrated volumetric fog, owned by VolumetricFogPass.
            {.binding = volumetricFogBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Clustered decals: the frame's decal records and per-cluster decal
            // lists, owned by DecalClusterPass.
            {.binding = DecalClusterPass::recordsBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = DecalClusterPass::clusterListsBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            // Opaque scene copy refracted by transmissive surfaces.
            {.binding = transmissionBackgroundBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
}
} // namespace mesh_pass_detail

struct MeshPass::FrameState {
    // Camera data every view starts from: the sun, the environment, the
    // irradiance volume, and the shadow cascades the leading view placed.
    mesh_pass_detail::CameraData base;
    DirectionalLightComponent light;
    EnvironmentLightComponent environment;
    std::optional<VolumetricFogComponent> fogMedium;
    glm::vec3 lightDirection{0.0f};
    uint32_t cascadeCount{0};
    bool shadowsEnabled{false};
    bool imageBasedLighting{false};
    EnvironmentFrameState environmentState;
    // Probes resident for per-pixel blending, matching environmentState.probeSlots.
    std::vector<ReflectionProbeVolume> residentProbes;
    // Local lights: every scene light, the leading view's selection, and the
    // first shadow tile of each selected light this frame rendered.
    uint32_t localLightLimit{0};
    std::vector<LocalLight> sceneLights;
    std::vector<LocalLight> leadLights;
    std::vector<int32_t> leadShadowTiles;
    ShadowCulling shadowCulling{ShadowCulling::Cpu};
    // Set once GPU-driven records were prepared for the default view.
    bool gpuDrivenPrepared{false};
};
