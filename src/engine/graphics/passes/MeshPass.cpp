#include "MeshPass.hpp"
#include "../DepthConvention.hpp"
#include "../IrradianceVolume.hpp"
#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"

#include "engine/assets/CompressedTexture.hpp"
#include "engine/assets/PackedVertexStreams.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"

#include <SDL3/SDL_stdinc.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>

namespace {
constexpr uint32_t shadowResolution = 1448;
constexpr uint32_t maximumShadowCascades = 3;
constexpr uint32_t maximumObjectsPerFrame = 16384;
constexpr uint32_t maximumSkinMatricesPerFrame = 65536;
constexpr uint32_t maximumMaterials = 4096;
constexpr uint32_t maximumLocalLights = 4096;
// Two slots (linear and sRGB view) per texture.
constexpr uint32_t maximumBindlessTextures = 8192;
// Slots kept free for the other sampled images bound alongside the array.
constexpr uint32_t reservedSampledImages = 16;
constexpr GeometryPageCapacity defaultGeometryPage{.vertices = 1u << 19, .indices = 1u << 21};
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
    // xy: integrated-volume uv per render pixel (1 / internal render extent).
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

// Mirrors MaterialRecord in shaders/mesh.frag and shaders/shadow.frag (std430).
struct MaterialRecord {
    glm::vec4 baseColor{1.0f};
    glm::vec4 factors{1.0f}; // metallic, roughness, normal scale, occlusion strength
    glm::vec4 emissive{0.0f};
    glm::vec4 surface{0.5f, 0.0f, 0.0f, 0.0f}; // alpha cutoff, alpha mode
    glm::uvec4 textures{0u};                   // base color, normal, metallic-roughness, occlusion
    glm::uvec4 extraTextures{0u};              // emissive
};

// Mirrors RenderObject in the mesh, shadow, outline, and selection shaders.
struct ObjectData {
    glm::mat4 model{1.0f};
    glm::mat4 previousModel{1.0f};
    // x: skin palette offset, y: joint count, z: material record index.
    glm::uvec4 skin{0};
};

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess, uint32_t layerCount = 1) {
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

// The primary fog medium with the lowest entity id, if the scene has one.
std::optional<VolumetricFogComponent> primaryVolumetricFog(const Scene& scene) {
    const auto components = scene.registry().view<const VolumetricFogComponent>();
    Entity selected = entt::null;
    for (Entity entity : components) {
        const VolumetricFogComponent& component = components.get<const VolumetricFogComponent>(entity);
        if (component.primary && (selected == entt::null || entt::to_integral(entity) < entt::to_integral(selected))) {
            selected = entity;
        }
    }
    if (selected == entt::null) {
        return std::nullopt;
    }
    return components.get<const VolumetricFogComponent>(selected);
}

template <typename Component> Component resolvePrimary(const Scene& scene) {
    const auto components = scene.registry().view<const Component>();
    Entity selected = entt::null;
    for (Entity entity : components) {
        const Component& component = components.template get<const Component>(entity);
        if (component.primary && (selected == entt::null || entt::to_integral(entity) < entt::to_integral(selected))) {
            selected = entity;
        }
    }
    return selected == entt::null ? Component{} : components.template get<const Component>(selected);
}

glm::vec3 normalizedLightDirection(const glm::vec3& direction) {
    constexpr float minimumDirectionLength = 0.0001f;
    if (glm::dot(direction, direction) < minimumDirectionLength * minimumDirectionLength) {
        return glm::normalize(DirectionalLightComponent{}.direction);
    }
    return glm::normalize(direction);
}

uint64_t handleKey(MeshHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

uint64_t handleKey(MaterialHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

uint64_t handleKey(TextureHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

uint64_t handleKey(GpuSceneHandle handle) {
    return (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
}

template <typename Handle> Handle handleFromKey(uint64_t key) {
    return {static_cast<uint32_t>(key), static_cast<uint32_t>(key >> 32)};
}

EnvironmentSource environmentSource(TextureHandle map, const AssetManager& assets,
                                    const EnvironmentLightComponent& environment) {
    const TextureAsset* asset = map ? assets.texture(map) : nullptr;
    return {.map = asset ? map : TextureHandle{},
            .mapAsset = asset,
            .skyColor = environment.skyColor,
            .groundColor = environment.groundColor};
}

bool perPixelReflectionProbes(const RenderSettings& settings) {
    return settings.environment != EnvironmentQuality::Hemisphere && settings.reflectionProbes &&
           settings.reflectionProbeBlending == ReflectionProbeBlending::PerPixel;
}

// Probes to keep resident for per-pixel blending: those with a loaded map,
// nearest the camera first, returned in entity order with their sources.
std::vector<ReflectionProbeVolume> selectPerPixelProbes(const Scene& scene, const AssetManager& assets,
                                                        const EnvironmentLightComponent& environment,
                                                        const glm::vec3& cameraPosition,
                                                        std::vector<EnvironmentSource>& sources) {
    std::vector<ReflectionProbeVolume> mapped;
    std::vector<EnvironmentSource> mappedSources;
    for (const ReflectionProbeVolume& probe : gatherReflectionProbes(scene)) {
        const auto& component = scene.registry().get<ReflectionProbeComponent>(probe.entity);
        const EnvironmentSource source = environmentSource(component.environmentMap, assets, environment);
        if (source.mapAsset) {
            mapped.push_back(probe);
            mappedSources.push_back(source);
        }
    }
    std::vector<ReflectionProbeVolume> selected;
    sources.clear();
    for (uint32_t index : selectResidentReflectionProbes(mapped, cameraPosition, maximumResidentReflectionProbes)) {
        selected.push_back(mapped[index]);
        sources.push_back(mappedSources[index]);
    }
    return selected;
}

// Image-based lighting uses the primary environment plus either the dominant
// reflection probe at the camera or, for per-pixel blending, the given
// resident probe sources; hemisphere lighting keeps only the analytic sky bound.
EnvironmentFrameRequest environmentRequest(const Scene& scene, const AssetManager& assets,
                                           const EnvironmentLightComponent& environment, const RenderSettings& settings,
                                           const glm::vec3& cameraPosition,
                                           std::vector<EnvironmentSource> residentProbes) {
    EnvironmentFrameRequest request{.quality = settings.environment};
    if (settings.environment == EnvironmentQuality::Hemisphere) {
        // Never sampled; fixed colors keep color edits from rebuilding the maps.
        const EnvironmentLightComponent defaults;
        request.primary = {.skyColor = defaults.skyColor, .groundColor = defaults.groundColor};
        return request;
    }
    request.primary = environmentSource(environment.environmentMap, assets, environment);
    if (!settings.reflectionProbes) {
        return request;
    }
    if (perPixelReflectionProbes(settings)) {
        request.residentProbes = std::move(residentProbes);
        return request;
    }
    // Probes arrive ordered by priority, then weight. Only the first with a loaded
    // map is bound; it fades over the environment across its blend distance.
    for (const ProbeBlend& blend : blendReflectionProbes(scene, cameraPosition)) {
        const auto& probe = scene.registry().get<ReflectionProbeComponent>(blend.entity);
        const EnvironmentSource source = environmentSource(probe.environmentMap, assets, environment);
        if (source.mapAsset) {
            request.probe = source;
            request.probeWeight = blend.influence;
            break;
        }
    }
    return request;
}
} // namespace

bool MeshPass::init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
                    VkExtent2D extent, VkImageView depthView) {
    m_device = context.device();
    m_pipelineCache = context.pipelineCache();
    m_allocator = context.vmaAllocator();
    m_blockCompressionSupported = context.blockCompressionSupported();
    m_packetCachingEnabled = SDL_getenv("ZENITH_DISABLE_RENDER_PACKET_CACHE") == nullptr;
    m_renderItems.reserve(maximumObjectsPerFrame);
    m_visibleItems.reserve(maximumObjectsPerFrame);
    m_shadowVisibleItems.reserve(maximumObjectsPerFrame);
    m_objectItems.reserve(maximumObjectsPerFrame);
    m_gpuRecords.reserve(maximumObjectsPerFrame);
    m_gpuHandleItems.reserve(maximumObjectsPerFrame);
    m_visibility.visibleInstances.reserve(maximumObjectsPerFrame);
    m_visibility.commands.reserve(maximumObjectsPerFrame);
    m_gpuDrivenObjects.reserve(maximumObjectsPerFrame);
    m_gpuDrivenBatches.reserve(maximumObjectsPerFrame);
    m_gpuDrivenCandidates.reserve(maximumObjectsPerFrame);
    m_gpuDrivenKeys.reserve(maximumObjectsPerFrame);
    if (!m_uploadContext.init(context, true) || !m_textureUploadContext.init(context, false) ||
        !createDescriptors(context, frameCount) ||
        !m_clusteredLighting.init(context, frameCount, ClusterGridConfiguration{}, maximumLocalLights) ||
        !m_reflectionProbeClusters.init(context, frameCount, m_clusteredLighting.configuration()) ||
        !m_volumetricFog.init(context, frameCount)) {
        return false;
    }
    writeClusteredLightingDescriptors();
    writeReflectionProbeDescriptors();
    // Written before the GPU-driven sets copy the shared bindings.
    m_fogDescriptorGenerations.assign(frameCount, 0);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        writeVolumetricFogDescriptors(frame);
    }
    TextureAsset white{.width = 1, .height = 1, .rgba = {255, 255, 255, 255}};
    TextureAsset normal{.width = 1, .height = 1, .rgba = {128, 128, 255, 255}};
    m_geometry = std::make_unique<GeometryArena>(defaultGeometryPage);
    if (!createZeroSkinInfluence() || !uploadTexture(white, m_whiteTexture) ||
        !uploadTexture(normal, m_normalTexture) || !createBindlessDescriptors(context, frameCount) ||
        !createShadowResources(depthFormat) ||
        !m_localShadows.init(context, frameCount, depthFormat, m_cameraLayout, m_bindlessLayout) ||
        !m_postProcess.init(context, colorFormat, depthFormat, frameCount, extent, depthView) ||
        !m_depthPyramid.init(context, extent, depthView) || !m_ambientOcclusion.init(context, frameCount, extent) ||
        !createPipeline(PostProcessPass::sceneFormat, depthFormat) ||
        !m_environmentPass.init(context, m_cameraLayout, PostProcessPass::sceneFormat, PostProcessPass::motionFormat,
                                depthFormat, frameCount)) {
        return false;
    }
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(context.pDevice(), &features);
    const GpuDrivenLimits limits{.maximumObjects = maximumObjectsPerFrame,
                                 .maximumInstances = maximumGpuDrivenInstances,
                                 .maximumBatches = maximumGpuDrivenInstances,
                                 .maximumGroups = maximumGpuDrawGroups,
                                 // Shadow cascades cull into their own regions.
                                 .frustumViews = maximumShadowCascades};
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        m_localShadows.writeDescriptors(frame, {&m_cameras[frame].descriptor, 1});
    }
    m_gpuDrivenSupported =
        m_gpuDrivenPass.init(context, frameCount, m_depthPyramid, limits, features12.drawIndirectCount == VK_TRUE);
    if (m_gpuDrivenSupported && !createGpuDrivenDescriptors()) {
        return false;
    }
    writeAmbientOcclusionDescriptors();
    if (!m_irradianceVolume.init(context, frameCount)) {
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const std::array<VkDescriptorSet, 2> sets{m_cameras[frame].descriptor, m_cameras[frame].gpuDrivenDescriptor};
        m_irradianceVolume.writeDescriptors(frame, sets);
    }
    return true;
}

bool MeshPass::createGpuDrivenDescriptors() {
    std::vector<VkDescriptorSetLayout> layouts(m_cameras.size(), m_cameraLayout);
    std::vector<VkDescriptorSet> sets(m_cameras.size());
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = m_descriptorPool,
                                         .descriptorSetCount = static_cast<uint32_t>(sets.size()),
                                         .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate GPU-driven camera descriptor sets")) {
        return false;
    }
    const VkDescriptorBufferInfo instances{.buffer = m_gpuDrivenPass.instanceBuffer(), .range = VK_WHOLE_SIZE};
    for (size_t frame = 0; frame < m_cameras.size(); ++frame) {
        CameraBuffer& camera = m_cameras[frame];
        camera.gpuDrivenDescriptor = sets[frame];
        // Everything except the object buffer matches the CPU-submission set.
        std::array<VkCopyDescriptorSet, 12> copies{};
        const uint32_t sharedBindings[]{0,
                                        1,
                                        3,
                                        4,
                                        9,
                                        10,
                                        11,
                                        previousSkinPaletteBinding,
                                        LocalShadowPass::atlasBinding,
                                        LocalShadowPass::tileBinding,
                                        ReflectionProbeClusterPass::cameraBinding,
                                        volumetricFogBinding};
        for (size_t index = 0; index < copies.size(); ++index) {
            copies[index] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                             .srcSet = camera.descriptor,
                             .srcBinding = sharedBindings[index],
                             .dstSet = camera.gpuDrivenDescriptor,
                             .dstBinding = sharedBindings[index],
                             .descriptorCount = 1};
        }
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = camera.gpuDrivenDescriptor,
                                         .dstBinding = 2,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &instances};
        vkUpdateDescriptorSets(m_device, 1, &write, static_cast<uint32_t>(copies.size()), copies.data());
    }
    return true;
}

void MeshPass::writeClusteredLightingDescriptors() {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const ClusteredLightingPass::FrameBuffers buffers = m_clusteredLighting.frameBuffers(frame);
        // Bindings 9-11: local lights, per-cluster ranges, cluster light indices.
        const std::array<VkDescriptorBufferInfo, 3> infos{{{.buffer = buffers.lights, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.clusterRanges, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.lightIndices, .range = VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = m_cameras[frame].descriptor,
                             .dstBinding = 9 + index,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[index]};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void MeshPass::writeReflectionProbeDescriptors() {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorBufferInfo probes{.buffer = m_reflectionProbeClusters.frameBuffer(frame),
                                            .range = VK_WHOLE_SIZE};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_cameras[frame].descriptor,
                                         .dstBinding = ReflectionProbeClusterPass::cameraBinding,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &probes};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void MeshPass::writeAmbientOcclusionDescriptors() {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorImageInfo occlusion{.sampler = m_ambientOcclusion.sampler(),
                                              .imageView = m_ambientOcclusion.occlusionView(frame),
                                              .imageLayout = AmbientOcclusionPass::occlusionLayout};
        // The GPU-driven set is written directly: copying would need the
        // CPU-submission set written first on every resize.
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t writeCount = 0;
        for (VkDescriptorSet set : {m_cameras[frame].descriptor, m_cameras[frame].gpuDrivenDescriptor}) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = set,
                                    .dstBinding = ambientOcclusionBinding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .pImageInfo = &occlusion};
        }
        vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    }
}

void MeshPass::writeVolumetricFogDescriptors(uint32_t frameIndex) {
    if (m_fogDescriptorGenerations[frameIndex] == m_volumetricFog.generation()) {
        return;
    }
    const VkDescriptorImageInfo fog{.sampler = m_volumetricFog.sampler(),
                                    .imageView = m_volumetricFog.integratedView(),
                                    .imageLayout = VolumetricFogPass::volumeLayout};
    // The frame slot's previous submission retired, so its sets are not in use.
    std::array<VkWriteDescriptorSet, 2> writes{};
    uint32_t writeCount = 0;
    for (VkDescriptorSet set : {m_cameras[frameIndex].descriptor, m_cameras[frameIndex].gpuDrivenDescriptor}) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = volumetricFogBinding,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &fog};
    }
    vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    m_fogDescriptorGenerations[frameIndex] = m_volumetricFog.generation();
}

bool MeshPass::resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView) {
    m_depthInitialized = false;
    m_cameraHistoryValid = false;
    m_temporalQuality.invalidateHistory();
    m_depthPrepassFrame.reset();
    if (!m_postProcess.resize(context, extent, depthView) || !m_depthPyramid.resize(extent, depthView) ||
        !m_ambientOcclusion.resize(extent)) {
        return false;
    }
    writeAmbientOcclusionDescriptors();
    if (m_gpuDrivenSupported) {
        m_gpuDrivenPass.bindDepthPyramid(m_depthPyramid);
    }
    return true;
}

bool MeshPass::createDescriptors(const VulkanContext& context, uint32_t frameCount) {
    const VkDescriptorSetLayoutBinding cameraBindings[]{
        {.binding = 0,
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
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(std::size(cameraBindings)),
        .pBindings = cameraBindings,
    };

    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_cameraLayout),
                     "Create camera descriptor layout")) {
        return false;
    }

    // Each frame has a CPU-submission camera set and a GPU-driven one that reads
    // instances from the culling output instead of the host object buffer.
    const VkDescriptorPoolSize poolSizes[]{
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frameCount * 2},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = frameCount * 2 * (8 + EnvironmentPass::environmentSlots + 1 /* irradiance volume */)},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = frameCount * 2 * (4 + EnvironmentPass::environmentSlots)}};
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = frameCount * 2,
        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
        .pPoolSizes = poolSizes,
    };

    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool),
                     "Create camera descriptor pool")) {
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    VkDescriptorSetAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descriptorPool,
        .descriptorSetCount = frameCount,
        .pSetLayouts = layouts.data(),
    };

    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()), "Allocate camera descriptor sets")) {
        return false;
    }

    m_cameras.resize(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i) {
        VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = sizeof(CameraData),
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};

        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].buffer.buffer,
                                         &m_cameras[i].buffer.allocation, &info),
                         "Create camera buffer")) {
            return false;
        }

        m_cameras[i].mapped = info.pMappedData;
        m_cameras[i].descriptor = sets[i];
        VkDescriptorBufferInfo descriptorBuffer{
            .buffer = m_cameras[i].buffer.buffer, .offset = 0, .range = sizeof(CameraData)};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = sets[i],
                                   .dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   .pBufferInfo = &descriptorBuffer};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        // Binding 3 is a storage buffer in the layout and the shaders.
        buffer.size = sizeof(glm::mat4) * maximumSkinMatricesPerFrame;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].skinPalettes.buffer,
                                         &m_cameras[i].skinPalettes.allocation, &info),
                         "Create per-frame skin palette buffer")) {
            return false;
        }
        m_cameras[i].skinPalettesMapped = info.pMappedData;
        VkDescriptorBufferInfo skinDescriptor{
            .buffer = m_cameras[i].skinPalettes.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 3;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &skinDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].previousSkinPalettes.buffer,
                                         &m_cameras[i].previousSkinPalettes.allocation, &info),
                         "Create per-frame previous skin palette buffer")) {
            return false;
        }
        m_cameras[i].previousSkinPalettesMapped = info.pMappedData;
        VkDescriptorBufferInfo previousSkinDescriptor{
            .buffer = m_cameras[i].previousSkinPalettes.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet previousSkinWrite = write;
        previousSkinWrite.dstBinding = previousSkinPaletteBinding;
        previousSkinWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        previousSkinWrite.pBufferInfo = &previousSkinDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &previousSkinWrite, 0, nullptr);

        buffer.size = sizeof(ObjectData) * maximumObjectsPerFrame * objectRegionCount;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].objects.buffer,
                                         &m_cameras[i].objects.allocation, &info),
                         "Create per-frame object buffer")) {
            return false;
        }
        m_cameras[i].objectsMapped = info.pMappedData;
        VkDescriptorBufferInfo objectDescriptor{
            .buffer = m_cameras[i].objects.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &objectDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        buffer.size = sizeof(MaterialRecord) * maximumMaterials;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].materials.buffer,
                                         &m_cameras[i].materials.allocation, &info),
                         "Create per-frame material buffer")) {
            return false;
        }
        m_cameras[i].materialsMapped = info.pMappedData;
        VkDescriptorBufferInfo materialDescriptor{
            .buffer = m_cameras[i].materials.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 4;
        write.pBufferInfo = &materialDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.pDevice(), &properties);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(context.pDevice(), &features);
    VkSamplerCreateInfo sampler{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = features.samplerAnisotropy,
        .maxAnisotropy = features.samplerAnisotropy ? std::min(8.0f, properties.limits.maxSamplerAnisotropy) : 1.0f,
        .maxLod = 16.0f};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create material sampler")) {
        return false;
    }

    return true;
}

bool MeshPass::createBindlessDescriptors(const VulkanContext& context, uint32_t frameCount) {
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(context.pDevice(), &features);
    VkPhysicalDeviceVulkan12Properties properties12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &properties12};
    vkGetPhysicalDeviceProperties2(context.pDevice(), &properties);
    // Update-after-bind is never used to modify live sets. It is requested only
    // when the ordinary sampled-image limits cannot hold the full table, since
    // it raises those limits on some vendors but also restricts tooling such as
    // frame capture.
    const auto& limits = properties.properties.limits;
    const uint32_t ordinaryLimit =
        std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
    const uint32_t updateAfterBindLimit = std::min(properties12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                   properties12.maxDescriptorSetUpdateAfterBindSampledImages);
    const bool updateAfterBind = ordinaryLimit < maximumBindlessTextures + reservedSampledImages &&
                                 features12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
                                 updateAfterBindLimit > ordinaryLimit;
    const uint32_t deviceLimit = updateAfterBind ? updateAfterBindLimit : ordinaryLimit;
    if (deviceLimit <= reservedSampledImages + 3) {
        std::cerr << "The device exposes too few sampled-image descriptors for bindless materials\n";
        return false;
    }
    const uint32_t capacity = std::min(maximumBindlessTextures, deviceLimit - reservedSampledImages);

    const VkDescriptorSetLayoutBinding bindings[]{{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                   .pImmutableSamplers = &m_sampler},
                                                  {.binding = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                   .descriptorCount = capacity,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    const VkDescriptorBindingFlags updateFlag = updateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0;
    const VkDescriptorBindingFlags bindingFlags[]{0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                         VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                                         updateFlag};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(std::size(bindingFlags)),
        .pBindingFlags = bindingFlags};
    VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .pNext = &flagsInfo,
                                               .flags = updateAfterBind
                                                            ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
                                                            : VkDescriptorSetLayoutCreateFlags{0},
                                               .bindingCount = static_cast<uint32_t>(std::size(bindings)),
                                               .pBindings = bindings};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_bindlessLayout),
                     "Create bindless material layout")) {
        return false;
    }
    const VkDescriptorPoolSize poolSizes[]{
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = frameCount},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = capacity * frameCount}};
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                                                                 : VkDescriptorPoolCreateFlags{0},
                                        .maxSets = frameCount,
                                        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
                                        .pPoolSizes = poolSizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_bindlessPool),
                     "Create bindless material pool")) {
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_bindlessLayout);
    std::vector<uint32_t> counts(frameCount, capacity);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCounts{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = frameCount,
        .pDescriptorCounts = counts.data()};
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .pNext = &variableCounts,
                                         .descriptorPool = m_bindlessPool,
                                         .descriptorSetCount = frameCount,
                                         .pSetLayouts = layouts.data()};
    std::vector<VkDescriptorSet> sets(frameCount);
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()), "Allocate bindless material sets")) {
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        m_cameras[frame].bindlessDescriptor = sets[frame];
    }

    m_textureTable = std::make_unique<BindlessTextureTable>(capacity, frameCount, m_whiteTexture.view);
    const auto white = m_textureTable->acquire(m_whiteTexture.view);
    const auto whiteSrgb = m_textureTable->acquire(m_whiteTexture.srgbView);
    const auto normal = m_textureTable->acquire(m_normalTexture.view);
    if (!white || !whiteSrgb || !normal) {
        return false;
    }
    m_whiteSlot = *white;
    m_whiteSrgbSlot = *whiteSrgb;
    m_normalSlot = *normal;
    return true;
}

bool MeshPass::createPipeline(VkFormat colorFormat, VkFormat depthFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule shadowVertex(m_device);
    ShaderModule shadowFragment(m_device);
    ShaderModule selectionMaskVertex(m_device);
    ShaderModule selectionMaskFragment(m_device);
    ShaderModule outlineVertex(m_device);
    ShaderModule outlineFragment(m_device);
    ShaderModule depthNormalVertex(m_device);
    ShaderModule depthNormalFragment(m_device);

    if (!shadowVertex.load(shaders / "shadow.vert.spv") || !shadowFragment.load(shaders / "shadow.frag.spv") ||
        !outlineVertex.load(shaders / "outline.vert.spv") || !outlineFragment.load(shaders / "outline.frag.spv") ||
        !selectionMaskVertex.load(shaders / "selection_mask.vert.spv") ||
        !selectionMaskFragment.load(shaders / "selection_mask.frag.spv") ||
        !depthNormalVertex.load(shaders / "depth_normal.vert.spv") ||
        !depthNormalFragment.load(shaders / "depth_normal.frag.spv")) {
        return false;
    }

    m_sceneColorFormat = colorFormat;
    m_sceneDepthFormat = depthFormat;
    const auto createDepthNormal = [&](GraphicsPipeline& pipeline, bool doubleSided) {
        return pipeline.initDepthNormalPipeline(
            m_device, PostProcessPass::normalFormat, PostProcessPass::specularFormat, depthFormat, m_cameraLayout,
            m_bindlessLayout, depthNormalVertex.handle(), depthNormalFragment.handle(), m_pipelineCache, doubleSided);
    };
    if (!createDepthNormal(m_depthNormalPipeline, false) ||
        !createDepthNormal(m_depthNormalDoubleSidedPipeline, true)) {
        return false;
    }
    return m_shadowPipeline.initShadowPipeline(m_device, depthFormat, m_cameraLayout, m_bindlessLayout,
                                               shadowVertex.handle(), shadowFragment.handle(), m_pipelineCache) &&
           m_selectionMaskPipeline.initSelectionMaskPipeline(m_device, colorFormat, PostProcessPass::motionFormat,
                                                             depthFormat, m_cameraLayout, selectionMaskVertex.handle(),
                                                             selectionMaskFragment.handle(), m_pipelineCache) &&
           m_outlinePipeline.initOutlinePipeline(m_device, colorFormat, PostProcessPass::motionFormat, depthFormat,
                                                 m_cameraLayout, outlineVertex.handle(), outlineFragment.handle(),
                                                 m_pipelineCache);
}

const MeshPass::MeshPipelineSet* MeshPass::meshPipelines(MeshShadingVariant variant) {
    if (const auto found = m_meshPipelines.find(variant); found != m_meshPipelines.end()) {
        return found->second.get();
    }
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "mesh.vert.spv") || !fragment.load(shaders / "mesh.frag.spv")) {
        return nullptr;
    }

    // Mirrors the constant_id declarations in shaders/mesh.frag.
    struct Constants {
        VkBool32 physicallyBased;
        int32_t shadowCascades;
    };

    const Constants constants{.physicallyBased = variant.physicallyBased ? VK_TRUE : VK_FALSE,
                              .shadowCascades = static_cast<int32_t>(variant.shadowCascades)};
    const VkSpecializationMapEntry entries[]{
        {.constantID = 0, .offset = offsetof(Constants, physicallyBased), .size = sizeof(VkBool32)},
        {.constantID = 1, .offset = offsetof(Constants, shadowCascades), .size = sizeof(int32_t)}};
    const VkSpecializationInfo specialization{.mapEntryCount = static_cast<uint32_t>(std::size(entries)),
                                              .pMapEntries = entries,
                                              .dataSize = sizeof(constants),
                                              .pData = &constants};
    auto pipelines = std::make_unique<MeshPipelineSet>();
    const SceneDepthUsage depthUsage =
        variant.depthPrepass ? SceneDepthUsage::EqualToPrepass : SceneDepthUsage::TestAndWrite;
    const auto create = [&](GraphicsPipeline& pipeline, bool doubleSided, bool blending) {
        return pipeline.initGraphicsPipeline(m_device, m_sceneColorFormat, PostProcessPass::motionFormat,
                                             m_sceneDepthFormat, m_cameraLayout, m_bindlessLayout, vertex.handle(),
                                             fragment.handle(), m_pipelineCache, doubleSided, blending, &specialization,
                                             depthUsage);
    };
    const bool created = create(pipelines->opaque, false, false) && create(pipelines->doubleSided, true, false) &&
                         create(pipelines->blend, false, true) && create(pipelines->blendDoubleSided, true, true);
    if (!created) {
        pipelines->opaque.shutdown(m_device);
        pipelines->doubleSided.shutdown(m_device);
        pipelines->blend.shutdown(m_device);
        pipelines->blendDoubleSided.shutdown(m_device);
        return nullptr;
    }
    return m_meshPipelines.emplace(variant, std::move(pipelines)).first->second.get();
}

bool MeshPass::createShadowResources(VkFormat depthFormat) {
    m_shadowFormat = depthFormat;
    VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = depthFormat,
                            .extent = {.width = shadowResolution, .height = shadowResolution, .depth = 1},
                            .mipLevels = 1,
                            .arrayLayers = maximumShadowCascades,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_shadowTexture.image,
                                    &m_shadowTexture.allocation, nullptr),
                     "Create shadow map")) {
        return false;
    }
    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = m_shadowTexture.image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                               .format = depthFormat,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                    .baseMipLevel = 0,
                                                    .levelCount = 1,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = maximumShadowCascades}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_shadowTexture.view), "Create shadow map view")) {
        return false;
    }
    for (uint32_t cascade = 0; cascade < maximumShadowCascades; ++cascade) {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange.baseArrayLayer = cascade;
        view.subresourceRange.layerCount = 1;
        if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_shadowLayerViews[cascade]),
                         "Create shadow cascade view")) {
            return false;
        }
    }
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_LINEAR,
                                .minFilter = VK_FILTER_LINEAR,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .compareEnable = VK_TRUE,
                                .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                .maxLod = 0.0f,
                                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_shadowSampler), "Create shadow sampler")) {
        return false;
    }
    VkDescriptorImageInfo imageInfo{.sampler = m_shadowSampler,
                                    .imageView = m_shadowTexture.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    for (const CameraBuffer& camera : m_cameras) {
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = camera.descriptor,
                                   .dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &imageInfo};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
    return true;
}

bool MeshPass::uploadBufferRegions(std::span<const BufferUploadRegion> regions) {
    const auto uploadBegin = std::chrono::steady_clock::now();
    constexpr VkDeviceSize stagingAlignment = 16;
    std::vector<VkDeviceSize> stagingOffsets;
    stagingOffsets.reserve(regions.size());
    VkDeviceSize stagingBytes = 0;
    for (const BufferUploadRegion& region : regions) {
        stagingOffsets.push_back(stagingBytes);
        stagingBytes += (region.size + stagingAlignment - 1) / stagingAlignment * stagingAlignment;
    }
    std::vector<std::byte> stagingData(static_cast<size_t>(stagingBytes));
    for (size_t index = 0; index < regions.size(); ++index) {
        std::memcpy(stagingData.data() + stagingOffsets[index], regions[index].source,
                    static_cast<size_t>(regions[index].size));
    }
    const VkBuffer staging = m_uploadContext.stage(stagingData.data(), stagingBytes);
    if (!staging) {
        return false;
    }
    const bool submitted = m_uploadContext.submit([&](VkCommandBuffer command) {
        for (size_t index = 0; index < regions.size(); ++index) {
            const VkBufferCopy copy{.srcOffset = stagingOffsets[index],
                                    .dstOffset = regions[index].destinationOffset,
                                    .size = regions[index].size};
            vkCmdCopyBuffer(command, staging, regions[index].destination, 1, &copy);
        }
    });
    if (submitted) {
        m_uploadGpuMilliseconds += m_uploadContext.lastGpuMilliseconds();
    }
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return submitted;
}

bool MeshPass::createGeometryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output) {
    VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const std::span<const uint32_t> queueFamilies = m_uploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        info.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateBuffer(m_allocator, &info, &allocation, &output.buffer, &output.allocation, nullptr),
                       "Create geometry buffer");
}

bool MeshPass::createZeroSkinInfluence() {
    const PackedSkinInfluence zero{};
    if (!createGeometryBuffer(sizeof(zero), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_zeroSkinInfluence)) {
        return false;
    }
    const BufferUploadRegion region{.source = &zero, .size = sizeof(zero), .destination = m_zeroSkinInfluence.buffer};
    return uploadBufferRegions({&region, 1});
}

bool MeshPass::ensureGeometryPage(uint32_t page) {
    while (m_geometryPages.size() <= page) {
        const uint32_t pageIndex = static_cast<uint32_t>(m_geometryPages.size());
        const VkDeviceSize vertices = m_geometry->pageCapacity(pageIndex).vertices;
        const VkDeviceSize indices = m_geometry->pageCapacity(pageIndex).indices;
        GeometryPage created{.layout = m_geometry->pageLayout(pageIndex)};
        const bool skinned = created.layout == GeometryLayout::Skinned;
        const bool allocated =
            createGeometryBuffer(vertex_packing::positionStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 created.positions) &&
            createGeometryBuffer(vertex_packing::attributeStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 created.attributes) &&
            (!skinned || createGeometryBuffer(vertex_packing::skinStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                              created.skin)) &&
            createGeometryBuffer(sizeof(uint32_t) * indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, created.indices);
        if (!allocated) {
            destroyGeometryPage(created);
            return false;
        }
        m_geometryPages.push_back(created);
    }
    return true;
}

void MeshPass::destroyGeometryPage(const GeometryPage& page) {
    for (const GpuBuffer* buffer : {&page.positions, &page.attributes, &page.skin, &page.indices}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(m_allocator, buffer->buffer, buffer->allocation);
        }
    }
}

bool MeshPass::uploadTexture(const TextureAsset& texture, GpuTexture& output) {
    if (texture.blockCompressed()) {
        return uploadBlockCompressedTexture(texture, output);
    }
    const auto uploadBegin = std::chrono::steady_clock::now();
    if (texture.width == 0 || texture.height == 0 ||
        texture.rgba.size() != static_cast<size_t>(texture.width) * texture.height * 4) {
        return false;
    }
    const VkDeviceSize byteCount = texture.rgba.size();
    const VkBuffer staging = m_textureUploadContext.stage(texture.rgba.data(), byteCount);
    if (!staging) {
        return false;
    }

    const uint32_t mipLevels =
        1 + static_cast<uint32_t>(std::floor(std::log2(std::max(texture.width, texture.height))));
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                                .mipLevels = mipLevels,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const std::span<const uint32_t> queueFamilies = m_textureUploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        imageInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo imageAllocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &imageInfo, &imageAllocation, &output.image, &output.allocation, nullptr),
            "Create material texture")) {
        return false;
    }

    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        const auto transitionMip = [&](uint32_t baseMip, uint32_t levelCount, VkImageLayout oldLayout,
                                       VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                       VkPipelineStageFlags2 destinationStage, VkAccessFlags2 sourceAccess,
                                       VkAccessFlags2 destinationAccess) {
            VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                          .srcStageMask = sourceStage,
                                          .srcAccessMask = sourceAccess,
                                          .dstStageMask = destinationStage,
                                          .dstAccessMask = destinationAccess,
                                          .oldLayout = oldLayout,
                                          .newLayout = newLayout,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = output.image,
                                          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                               .baseMipLevel = baseMip,
                                                               .levelCount = levelCount,
                                                               .baseArrayLayer = 0,
                                                               .layerCount = 1}};
            VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                        .imageMemoryBarrierCount = 1,
                                        .pImageMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(command, &dependency);
        };
        transitionMip(0, mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .mipLevel = 0,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1},
                               .imageExtent = {.width = texture.width, .height = texture.height, .depth = 1}};
        vkCmdCopyBufferToImage(command, staging, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        int32_t sourceWidth = static_cast<int32_t>(texture.width);
        int32_t sourceHeight = static_cast<int32_t>(texture.height);
        for (uint32_t mip = 1; mip < mipLevels; ++mip) {
            transitionMip(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            const int32_t destinationWidth = std::max(sourceWidth / 2, 1);
            const int32_t destinationHeight = std::max(sourceHeight / 2, 1);
            VkImageBlit blit{.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .mipLevel = mip - 1,
                                                .baseArrayLayer = 0,
                                                .layerCount = 1},
                             .srcOffsets = {{0, 0, 0}, {sourceWidth, sourceHeight, 1}},
                             .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .mipLevel = mip,
                                                .baseArrayLayer = 0,
                                                .layerCount = 1},
                             .dstOffsets = {{0, 0, 0}, {destinationWidth, destinationHeight, 1}}};
            vkCmdBlitImage(command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            transitionMip(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          m_textureUploadContext.usesDedicatedQueue() ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                                      : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT,
                          m_textureUploadContext.usesDedicatedQueue() ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        transitionMip(mipLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      m_textureUploadContext.usesDedicatedQueue() ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                                  : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      m_textureUploadContext.usesDedicatedQueue() ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (uploaded) {
        m_uploadGpuMilliseconds += m_textureUploadContext.lastGpuMilliseconds();
    }
    if (!uploaded) {
        return false;
    }

    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = output.image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .baseMipLevel = 0,
                                                    .levelCount = mipLevels,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.view), "Create material texture view")) {
        return false;
    }
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    const bool created =
        checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.srgbView), "Create sRGB material texture view");
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return created;
}

// Cooked textures carry their full mip chain, so every level is copied as-is.
// BC7 gets UNORM and sRGB views; BC5 has no sRGB variant and gets one view.
bool MeshPass::uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output) {
    // ensureTexture already rejected unsupported or invalid encodings.
    const auto uploadBegin = std::chrono::steady_clock::now();
    const bool bc7 = texture.format == TextureFormat::Bc7;
    const uint32_t mipLevels = static_cast<uint32_t>(texture.mips.size());
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = bc7 ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0u,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK,
                                .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                                .mipLevels = mipLevels,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const std::span<const uint32_t> queueFamilies = m_textureUploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        imageInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo imageAllocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &imageInfo, &imageAllocation, &output.image, &output.allocation, nullptr),
            "Create block-compressed material texture")) {
        output = {};
        return false;
    }
    // Nothing references the image until this returns, so failures release it directly.
    const auto fail = [&] {
        destroyTexture(output);
        output = {};
        return false;
    };
    // Stage only after the image exists: a staged slot must be submitted.
    const VkBuffer staging = m_textureUploadContext.stage(texture.compressed.data(), texture.compressed.size());
    if (!staging) {
        return fail();
    }

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(mipLevels);
    for (uint32_t level = 0; level < mipLevels; ++level) {
        const TextureMipLevel& mip = texture.mips[level];
        copies.push_back({.bufferOffset = mip.offset,
                          .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                               .mipLevel = level,
                                               .baseArrayLayer = 0,
                                               .layerCount = 1},
                          .imageExtent = {.width = mip.width, .height = mip.height, .depth = 1}});
    }
    const VkImageSubresourceRange allLevels{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .baseMipLevel = 0,
                                            .levelCount = mipLevels,
                                            .baseArrayLayer = 0,
                                            .layerCount = 1};
    const bool dedicatedQueue = m_textureUploadContext.usesDedicatedQueue();
    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        const auto transition = [&](VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                    VkPipelineStageFlags2 destinationStage, VkAccessFlags2 sourceAccess,
                                    VkAccessFlags2 destinationAccess) {
            const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                                .srcStageMask = sourceStage,
                                                .srcAccessMask = sourceAccess,
                                                .dstStageMask = destinationStage,
                                                .dstAccessMask = destinationAccess,
                                                .oldLayout = oldLayout,
                                                .newLayout = newLayout,
                                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                .image = output.image,
                                                .subresourceRange = allLevels};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .imageMemoryBarrierCount = 1,
                                              .pImageMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(command, &dependency);
        };
        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(command, staging, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copies.size()), copies.data());
        // Matches the RGBA8 path: graphics-queue consumers wait on the upload
        // timeline semaphore when the copy ran on a dedicated transfer queue.
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   dedicatedQueue ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, dedicatedQueue ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!uploaded) {
        return fail();
    }
    m_uploadGpuMilliseconds += m_textureUploadContext.lastGpuMilliseconds();

    // The copy may still be in flight from here on, so view failures keep the
    // image alive rather than destroying it under the transfer queue.
    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = output.image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = imageInfo.format,
                               .subresourceRange = allLevels};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.view),
                     "Create block-compressed texture view")) {
        return false;
    }
    bool created = true;
    if (bc7) {
        view.format = VK_FORMAT_BC7_SRGB_BLOCK;
        created = checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.srgbView),
                              "Create sRGB block-compressed texture view");
    }
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return created;
}

const MeshPass::GpuMesh* MeshPass::ensureMesh(MeshHandle handle, const AssetManager& assets) {
    const uint64_t key = handleKey(handle);
    if (const auto found = m_meshes.find(key); found != m_meshes.end()) {
        return &found->second;
    }
    const MeshAsset* mesh = assets.mesh(handle);
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty() || m_unpackableMeshes.contains(key)) {
        return nullptr;
    }
    std::string packError;
    const std::optional<PackedVertexStreams> streams = vertex_packing::packVertexStreams(mesh->vertices, packError);
    if (!streams) {
        std::cerr << "Mesh vertices cannot be packed for upload: " << packError << '\n';
        m_unpackableMeshes.insert(key);
        return nullptr;
    }
    // Matches uploadBufferRegions' 16-byte region alignment. A mesh larger than
    // one staging slot can never upload, so it is not retried as a deferral.
    const auto alignedBytes = [](size_t bytes) { return (static_cast<VkDeviceSize>(bytes) + 15) / 16 * 16; };
    const VkDeviceSize stagingBytes = alignedBytes(streams->positions.size() * sizeof(glm::vec3)) +
                                      alignedBytes(streams->attributes.size() * sizeof(PackedVertexAttributes)) +
                                      alignedBytes(mesh->indices.size() * sizeof(uint32_t)) +
                                      alignedBytes(streams->skin.size() * sizeof(PackedSkinInfluence));
    if (stagingBytes > UploadContext::maximumSlotBytes) {
        std::cerr << "Mesh rejected: " << stagingBytes << " bytes exceed the " << UploadContext::maximumSlotBytes
                  << "-byte upload limit\n";
        m_unpackableMeshes.insert(key);
        return nullptr;
    }
    const auto geometry =
        m_geometry->allocate(static_cast<uint32_t>(mesh->vertices.size()), static_cast<uint32_t>(mesh->indices.size()),
                             streams->skinned() ? GeometryLayout::Skinned : GeometryLayout::Static);
    if (!geometry) {
        // Retried every frame while the arena is full; logged once per episode.
        if (!m_geometryArenaFullReported) {
            std::cerr << "Geometry arena could not place a mesh; its draws are deferred until space frees\n";
            m_geometryArenaFullReported = true;
        }
        m_deferredUploads.markDeferred(DeferredUploadKind::Mesh, key);
        return nullptr;
    }
    m_geometryArenaFullReported = false;
    bool uploaded = ensureGeometryPage(geometry->page);
    if (uploaded) {
        const GeometryPage& page = m_geometryPages[geometry->page];
        const auto firstVertex = static_cast<VkDeviceSize>(geometry->vertexOffset);
        std::vector<BufferUploadRegion> regions{{.source = streams->positions.data(),
                                                 .size = streams->positions.size() * sizeof(glm::vec3),
                                                 .destination = page.positions.buffer,
                                                 .destinationOffset = vertex_packing::positionStride * firstVertex},
                                                {.source = streams->attributes.data(),
                                                 .size = streams->attributes.size() * sizeof(PackedVertexAttributes),
                                                 .destination = page.attributes.buffer,
                                                 .destinationOffset = vertex_packing::attributeStride * firstVertex},
                                                {.source = mesh->indices.data(),
                                                 .size = mesh->indices.size() * sizeof(uint32_t),
                                                 .destination = page.indices.buffer,
                                                 .destinationOffset = sizeof(uint32_t) * geometry->firstIndex}};
        if (streams->skinned()) {
            regions.push_back({.source = streams->skin.data(),
                               .size = streams->skin.size() * sizeof(PackedSkinInfluence),
                               .destination = page.skin.buffer,
                               .destinationOffset = vertex_packing::skinStride * firstVertex});
        }
        uploaded = uploadBufferRegions(regions);
    }
    if (!uploaded) {
        // Typically staging backpressure while a scene loads; retried next frame.
        m_geometry->free(*geometry);
        m_deferredUploads.markDeferred(DeferredUploadKind::Mesh, key);
        return nullptr;
    }
    m_deferredUploads.markUploaded(DeferredUploadKind::Mesh, key);
    return &m_meshes.emplace(key, GpuMesh{*geometry}).first->second;
}

std::string MeshPass::textureRejectionReason(const TextureAsset& texture) const {
    const auto exceedsStaging = [](size_t bytes) { return bytes > UploadContext::maximumSlotBytes; };
    if (texture.blockCompressed()) {
        if (!m_blockCompressionSupported) {
            return "the device cannot sample BC formats";
        }
        if (const AssetPipelineResult valid = validateBlockCompressedTexture(texture); !valid) {
            return valid.error;
        }
        if (exceedsStaging(texture.compressed.size())) {
            return "compressed data exceeds the upload limit";
        }
        return {};
    }
    if (texture.width == 0 || texture.height == 0 ||
        texture.rgba.size() != static_cast<size_t>(texture.width) * texture.height * 4) {
        return "RGBA8 pixel data does not match the texture dimensions";
    }
    if (exceedsStaging(texture.rgba.size())) {
        return "RGBA8 pixel data exceeds the upload limit";
    }
    return {};
}

const MeshPass::GpuTexture* MeshPass::ensureTexture(TextureHandle handle, const AssetManager& assets) {
    const uint64_t key = (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
    if (const auto found = m_textures.find(key); found != m_textures.end()) {
        return &found->second;
    }
    const TextureAsset* texture = assets.texture(handle);
    if (!texture || m_rejectedTextures.contains(key)) {
        return nullptr;
    }
    // Permanent failures are remembered so they are neither retried nor
    // logged every frame; only retryable failures count as deferred.
    if (const std::string reason = textureRejectionReason(*texture); !reason.empty()) {
        std::cerr << "Texture rejected: " << reason << "; materials sample a fallback texture\n";
        m_rejectedTextures.insert(key);
        return nullptr;
    }
    GpuTexture gpu;
    if (!uploadTexture(*texture, gpu)) {
        m_deferredUploads.markDeferred(DeferredUploadKind::Texture, key);
        return nullptr;
    }
    if (!assignTextureSlots(gpu)) {
        // Nothing has referenced the slots or views yet, so release directly.
        destroyTexture(gpu);
        m_deferredUploads.markDeferred(DeferredUploadKind::Texture, key);
        return nullptr;
    }
    m_deferredUploads.markUploaded(DeferredUploadKind::Texture, key);
    return &m_textures.emplace(key, gpu).first->second;
}

bool MeshPass::assignTextureSlots(GpuTexture& texture) {
    const auto linear = m_textureTable->acquire(texture.view);
    if (linear && !texture.srgbView) {
        // Formats without an sRGB variant (BC5) sample linearly in every slot.
        texture.linearSlot = *linear;
        texture.srgbSlot = *linear;
        m_textureTableFullReported = false;
        return true;
    }
    const auto srgb = linear ? m_textureTable->acquire(texture.srgbView) : std::nullopt;
    if (!linear || !srgb) {
        if (linear) {
            m_textureTable->release(*linear);
        }
        // Retried every frame while the table is full; logged once per episode.
        if (!m_textureTableFullReported) {
            std::cerr << "Bindless texture table is full (" << m_textureTable->capacity()
                      << " slots); textures are deferred until slots free\n";
            m_textureTableFullReported = true;
        }
        return false;
    }
    m_textureTableFullReported = false;
    texture.linearSlot = *linear;
    texture.srgbSlot = *srgb;
    return true;
}

void MeshPass::retireTexture(const GpuTexture& texture) {
    // The slot is rewritten in every frame set before that set is bound again;
    // the views die only after all frames that could still bind them retire.
    m_textureTable->release(texture.linearSlot);
    if (texture.srgbSlot != texture.linearSlot) {
        m_textureTable->release(texture.srgbSlot);
    }
    m_releaseQueue.defer(m_frameSerial, [this, texture] { destroyTexture(texture); });
}

void MeshPass::destroyTexture(const GpuTexture& texture) {
    if (texture.srgbView) {
        vkDestroyImageView(m_device, texture.srgbView, nullptr);
    }
    if (texture.view) {
        vkDestroyImageView(m_device, texture.view, nullptr);
    }
    if (texture.image) {
        vmaDestroyImage(m_allocator, texture.image, texture.allocation);
    }
}

const MeshPass::GpuMaterial* MeshPass::ensureMaterial(MaterialHandle handle, const AssetManager& assets) {
    const uint64_t key = handleKey(handle);
    if (const auto found = m_materials.find(key); found != m_materials.end()) {
        return &found->second;
    }
    const MaterialAsset* material = assets.material(handle);
    if (!material) {
        return nullptr;
    }
    uint32_t index = 0;
    if (!m_freeMaterialIndices.empty()) {
        index = m_freeMaterialIndices.back();
        m_freeMaterialIndices.pop_back();
    } else if (m_nextMaterialIndex < maximumMaterials) {
        index = m_nextMaterialIndex++;
    } else {
        std::cerr << "Material count exceeds the renderer capacity (" << maximumMaterials << ")\n";
        return nullptr;
    }
    // Upload referenced textures now so frame recording only resolves slots.
    for (const TextureHandle texture :
         {material->baseColorTexture, material->normalTexture, material->metallicRoughnessTexture,
          material->occlusionTexture, material->emissiveTexture}) {
        if (texture) {
            ensureTexture(texture, assets);
        }
    }
    return &m_materials.emplace(key, GpuMaterial{index}).first->second;
}

void MeshPass::writeMaterialRecords(uint32_t frameIndex, const AssetManager& assets) {
    auto* records = static_cast<MaterialRecord*>(m_cameras[frameIndex].materialsMapped);
    const auto slotFor = [&](TextureHandle handle, bool srgb, uint32_t fallback) {
        if (!handle) {
            return fallback;
        }
        // A texture missing here failed its first upload, typically because the
        // staging ring was full while a large batch of materials loaded. Retry it
        // every frame until it lands; the fallback covers the frames in between.
        const GpuTexture* texture = ensureTexture(handle, assets);
        if (!texture) {
            return fallback;
        }
        return srgb ? texture->srgbSlot : texture->linearSlot;
    };
    uint32_t highestIndex = 0;
    for (const auto& [key, gpu] : m_materials) {
        const MaterialAsset* material = assets.material(handleFromKey<MaterialHandle>(key));
        if (!material) {
            continue;
        }
        // Rewritten every frame so edits to factors or texture bindings apply
        // without invalidating anything the GPU might still be reading.
        records[gpu.index] = {
            .baseColor = material->baseColor,
            .factors = {material->metallic, material->roughness, material->normalScale, material->occlusionStrength},
            .emissive = {material->emissive * material->emissiveStrength, 0.0f},
            .surface = {material->alphaCutoff, static_cast<float>(material->alphaMode), 0.0f, 0.0f},
            .textures = {slotFor(material->baseColorTexture, true, m_whiteSrgbSlot),
                         slotFor(material->normalTexture, false, m_normalSlot),
                         slotFor(material->metallicRoughnessTexture, false, m_whiteSlot),
                         slotFor(material->occlusionTexture, false, m_whiteSlot)},
            .extraTextures = {slotFor(material->emissiveTexture, true, m_whiteSrgbSlot), 0u, 0u, 0u}};
        highestIndex = std::max(highestIndex, gpu.index + 1);
    }
    if (highestIndex > 0) {
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].materials.allocation, 0,
                           sizeof(MaterialRecord) * highestIndex);
    }
}

void MeshPass::updateBindlessDescriptor(uint32_t frameIndex) {
    const std::vector<uint32_t> slots = m_textureTable->takePendingWrites(frameIndex);
    if (slots.empty()) {
        return;
    }
    std::vector<VkDescriptorImageInfo> images(slots.size());
    std::vector<VkWriteDescriptorSet> writes(slots.size());
    for (size_t index = 0; index < slots.size(); ++index) {
        images[index] = {.imageView = m_textureTable->view(slots[index]),
                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = m_cameras[frameIndex].bindlessDescriptor,
                         .dstBinding = 1,
                         .dstArrayElement = slots[index],
                         .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                         .pImageInfo = &images[index]};
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void MeshPass::bindGeometry(VkCommandBuffer commandBuffer, const GpuMesh& mesh,
                            std::optional<uint32_t>& boundPage) const {
    if (boundPage == mesh.geometry.page) {
        return;
    }
    bindGeometryPage(commandBuffer, mesh.geometry.page);
    boundPage = mesh.geometry.page;
}

void MeshPass::bindGeometryPage(VkCommandBuffer commandBuffer, uint32_t pageIndex) const {
    static_assert(mesh_vertex_binding::position == 0 && mesh_vertex_binding::attributes == 1 &&
                      mesh_vertex_binding::skin == 2,
                  "stream buffers are bound in binding order");
    const GeometryPage& page = m_geometryPages[pageIndex];
    const bool skinned = page.layout == GeometryLayout::Skinned;
    const std::array<VkBuffer, mesh_vertex_binding::count> buffers{
        page.positions.buffer, page.attributes.buffer, skinned ? page.skin.buffer : m_zeroSkinInfluence.buffer};
    const std::array<VkDeviceSize, mesh_vertex_binding::count> offsets{};
    // Static pages repeat one zero influence for every vertex, so the shaders'
    // zero-weight path leaves them unskinned.
    const std::array<VkDeviceSize, mesh_vertex_binding::count> strides{
        vertex_packing::positionStride, vertex_packing::attributeStride, skinned ? vertex_packing::skinStride : 0u};
    vkCmdBindVertexBuffers2(commandBuffer, 0, mesh_vertex_binding::count, buffers.data(), offsets.data(), nullptr,
                            strides.data());
    vkCmdBindIndexBuffer(commandBuffer, page.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
}

void MeshPass::beginRendering(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView motionView,
                              VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                              SceneAttachments attachments) const {
    const bool clear = attachments == SceneAttachments::Clear;
    // A preserved pass continues one the caller already transitioned.
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage);
    }
    const VkAttachmentLoadOp loadOp =
        attachments == SceneAttachments::Preserve ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    const VkAttachmentLoadOp depthLoadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = colorView,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = loadOp,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {.float32 = {0.015f, 0.02f, 0.04f, 0.0f}}}};
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = depthView,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = depthLoadOp,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.depthStencil = {.depth = scene_depth::clearValue, .stencil = 0}}};
    VkRenderingAttachmentInfo motion{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = motionView,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = loadOp,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}};
    const VkRenderingAttachmentInfo colors[]{color, motion};
    VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {.offset = {0, 0}, .extent = extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 2,
                              .pColorAttachments = colors,
                              .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
}

void MeshPass::prepareDepthForClear(VkCommandBuffer commandBuffer, VkImage depthImage) const {
    // Last frame's post-processing sampled the depth it stored.
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 m_depthInitialized ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 m_depthInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 m_depthInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void MeshPass::beginDepthPrepass(VkCommandBuffer commandBuffer, VkImageView normalView, VkImageView specularView,
                                 VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                                 SceneAttachments attachments) const {
    const bool clear = attachments == SceneAttachments::Clear;
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage);
    }
    const VkAttachmentLoadOp loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkRenderingAttachmentInfo colors[]{{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = normalView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = loadOp,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}},
                                             // A null view discards the shader's specular output.
                                             {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = specularView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = loadOp,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
    const VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = loadOp,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {.depth = scene_depth::clearValue, .stencil = 0}}};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = extent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = static_cast<uint32_t>(std::size(colors)),
                                    .pColorAttachments = colors,
                                    .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
}

void MeshPass::setSceneViewport(const DrawContext& draw) const {
    const VkViewport viewport{.x = 0,
                              .y = 0,
                              .width = static_cast<float>(draw.renderExtent.width),
                              .height = static_cast<float>(draw.renderExtent.height),
                              .minDepth = 0,
                              .maxDepth = 1};
    const VkRect2D scissor{.offset = {0, 0}, .extent = draw.renderExtent};
    vkCmdSetViewport(draw.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(draw.commandBuffer, 0, 1, &scissor);
}

bool MeshPass::buildRenderItems(const Scene& scene, const AssetManager& assets,
                                const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items) {
    m_meshUploadsPending = false;
    const auto renderables = scene.registry().view<const TransformComponent, const MeshRendererComponent>();
    for (Entity entity : renderables) {
        const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(entity));
        const bool outlined = std::binary_search(outlinedObjects.begin(), outlinedObjects.end(), objectId);
        const MeshHandle meshHandle = renderables.get<const MeshRendererComponent>(entity).mesh;
        const MeshAsset* mesh = assets.mesh(meshHandle);
        if (!mesh) {
            return false;
        }
        // A mesh whose upload could not be staged this frame (the staging ring is
        // full while a scene loads) is skipped and retried on the next frame;
        // the pending flag forces that rebuild even when nothing else changed.
        const GpuMesh* gpuMesh = ensureMesh(meshHandle, assets);
        if (!gpuMesh) {
            m_meshUploadsPending = m_meshUploadsPending ||
                                   m_deferredUploads.deferredThisFrame(DeferredUploadKind::Mesh, handleKey(meshHandle));
            continue;
        }
        const glm::mat4 world = renderables.get<const TransformComponent>(entity).world;
        const SkinComponent* skin = scene.registry().try_get<SkinComponent>(entity);
        const glm::vec3 center = glm::vec3(world * glm::vec4(mesh->bounds.center, 1.0f));
        const float scale = std::max(
            {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
        for (const MeshPrimitive& primitive : mesh->primitives) {
            const MaterialAsset* material = assets.material(primitive.material);
            const GpuMaterial* gpuMaterial = ensureMaterial(primitive.material, assets);
            if (!material || !gpuMaterial) {
                return false;
            }
            items.push_back({.meshHandle = meshHandle,
                             .entity = entity,
                             .materialHandle = primitive.material,
                             .mesh = mesh,
                             .primitive = &primitive,
                             .lod = primitive.lod(0),
                             .material = material,
                             .gpuMesh = gpuMesh,
                             .gpuMaterial = gpuMaterial,
                             .world = world,
                             .previousWorld = world,
                             .boundsCenter = center,
                             .boundsRadius = mesh->bounds.radius * scale,
                             .outlined = outlined,
                             .jointPalette = skin ? skin->jointPalette : std::vector<glm::mat4>{}});
        }
    }
    std::sort(items.begin(), items.end(), [](const RenderItem& first, const RenderItem& second) {
        if (first.material->alphaMode != second.material->alphaMode) {
            return first.material->alphaMode < second.material->alphaMode;
        }
        if (first.material->doubleSided != second.material->doubleSided) {
            return first.material->doubleSided < second.material->doubleSided;
        }
        if (handleKey(first.materialHandle) != handleKey(second.materialHandle)) {
            return handleKey(first.materialHandle) < handleKey(second.materialHandle);
        }
        if (handleKey(first.meshHandle) != handleKey(second.meshHandle)) {
            return handleKey(first.meshHandle) < handleKey(second.meshHandle);
        }
        if (first.primitive->firstIndex != second.primitive->firstIndex) {
            return first.primitive->firstIndex < second.primitive->firstIndex;
        }
        return first.outlined < second.outlined;
    });
    m_objectItems.clear();
    std::unordered_map<uint64_t, GpuSceneHandle> nextGpuRecords;
    nextGpuRecords.reserve(items.size());
    m_gpuHandleItems.clear();
    for (size_t index = 0; index < items.size(); ++index) {
        RenderItem& item = items[index];
        const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(item.entity));
        m_objectItems[objectId].push_back(index);
        item.recordId = static_cast<uint64_t>(objectId) * 0x9e3779b97f4a7c15ull;
        item.recordId ^=
            handleKey(item.meshHandle) + 0x9e3779b97f4a7c15ull + (item.recordId << 6) + (item.recordId >> 2);
        item.recordId ^=
            handleKey(item.materialHandle) + item.primitive->firstIndex + (item.recordId << 6) + (item.recordId >> 2);
        const GpuSceneObject gpuObject{.recordId = item.recordId,
                                       .objectId = objectId,
                                       .meshKey = handleKey(item.meshHandle),
                                       .materialKey = handleKey(item.materialHandle),
                                       .world = item.world,
                                       .localBoundsCenter = item.mesh->bounds.center,
                                       .localBoundsRadius = item.mesh->bounds.radius,
                                       .indexCount = item.primitive->indexCount,
                                       .firstIndex = item.primitive->firstIndex};
        item.gpuSceneHandle = m_gpuScene.upsert(gpuObject);
        nextGpuRecords.emplace(item.recordId, item.gpuSceneHandle);
        m_gpuHandleItems.emplace(handleKey(item.gpuSceneHandle), index);
    }
    for (const auto& [recordId, handle] : m_gpuRecords) {
        if (!nextGpuRecords.contains(recordId)) {
            m_gpuScene.remove(handle);
        }
    }
    m_gpuRecords.swap(nextGpuRecords);
    return true;
}

uint32_t MeshPass::updateRenderItems(const Scene& scene, std::span<const Entity> changedEntities) {
    uint32_t updated = 0;
    for (Entity entity : changedEntities) {
        if (!scene.registry().valid(entity)) {
            continue;
        }
        const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(entity));
        const auto found = m_objectItems.find(objectId);
        const TransformComponent* transform = scene.registry().try_get<TransformComponent>(entity);
        if (found == m_objectItems.end() || transform == nullptr) {
            continue;
        }
        const SkinComponent* skin = scene.registry().try_get<SkinComponent>(entity);
        for (size_t itemIndex : found->second) {
            RenderItem& item = m_renderItems[itemIndex];
            item.previousWorld = item.world;
            item.world = transform->world;
            item.boundsCenter = glm::vec3(item.world * glm::vec4(item.mesh->bounds.center, 1.0f));
            const float scale = std::max({glm::length(glm::vec3(item.world[0])), glm::length(glm::vec3(item.world[1])),
                                          glm::length(glm::vec3(item.world[2]))});
            item.boundsRadius = item.mesh->bounds.radius * scale;
            if (skin) {
                item.jointPalette = skin->jointPalette;
            }
            m_gpuScene.update(item.gpuSceneHandle, {.recordId = item.recordId,
                                                    .objectId = objectId,
                                                    .meshKey = handleKey(item.meshHandle),
                                                    .materialKey = handleKey(item.materialHandle),
                                                    .world = item.world,
                                                    .localBoundsCenter = item.mesh->bounds.center,
                                                    .localBoundsRadius = item.mesh->bounds.radius,
                                                    .indexCount = item.primitive->indexCount,
                                                    .firstIndex = item.primitive->firstIndex});
            ++updated;
        }
    }
    return updated;
}

void MeshPass::recordLocalShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                                  const AssetManager& assets, const RenderSettings& settings,
                                  std::span<const LocalLight> lights, const glm::vec3& cameraPosition,
                                  OptionalPassTimestamps& timestamps, RenderStats& stats) {
    const auto cpuBegin = std::chrono::steady_clock::now();
    const SceneRevisions& revisions = scene.revisions();
    collectShadowCasterChanges(scene);
    const bool enabled = settings.localLights != LocalLightingQuality::Disabled;
    const LocalShadowFrameInput input{.quality = enabled ? settings.localShadows : LocalShadowQuality::Off,
                                      .lights = lights,
                                      .cameraPosition = cameraPosition,
                                      .changedCasters = m_changedShadowCasters,
                                      .invalidateAll = revisions.renderable != m_localShadowRevisions.renderable ||
                                                       revisions.material != m_localShadowRevisions.material ||
                                                       assets.revision() != m_localShadowAssetRevision};
    const CameraBuffer& camera = m_cameras[frameIndex];
    const std::array<VkDescriptorSet, 2> cameraSets{camera.descriptor, camera.gpuDrivenDescriptor};
    m_localShadowObjectCount = 0;
    const std::vector<int32_t> firstTiles = m_localShadows.record(
        commandBuffer, frameIndex, m_frameSerial, input,
        std::span<const VkDescriptorSet>(cameraSets.data(), camera.gpuDrivenDescriptor ? 2u : 1u),
        camera.bindlessDescriptor, m_releaseQueue, timestamps,
        [&](VkCommandBuffer tileCommands, const glm::mat4& viewProjection) {
            return drawLocalShadowCasters(tileCommands, frameIndex, viewProjection, stats);
        },
        stats);
    m_clusteredLighting.assignShadowTiles(frameIndex, firstTiles);
    m_localShadowRevisions = revisions;
    m_localShadowAssetRevision = assets.revision();
    stats.cpuLocalShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuBegin).count();
}

void MeshPass::collectShadowCasterChanges(const Scene& scene) {
    m_changedShadowCasters.clear();
    const auto addCaster = [&](const RenderItem& item) {
        if (item.material->alphaMode == MaterialAlphaMode::Blend) {
            return;
        }
        // updateRenderItems kept the previous transform, so both the vacated
        // and the newly covered space are reported.
        m_changedShadowCasters.push_back({item.boundsCenter, item.boundsRadius});
        m_changedShadowCasters.push_back({glm::vec3(item.previousWorld * glm::vec4(item.mesh->bounds.center, 1.0f)),
                                          item.mesh->bounds.radius * maximumAxisScale(item.previousWorld)});
    };
    const SceneRevisions& revisions = scene.revisions();
    if (revisions.transform != m_localShadowRevisions.transform) {
        for (Entity entity : scene.changedTransforms()) {
            const auto found = m_objectItems.find(static_cast<RenderObjectId>(entt::to_integral(entity)));
            if (found == m_objectItems.end()) {
                continue;
            }
            for (size_t itemIndex : found->second) {
                addCaster(m_renderItems[itemIndex]);
            }
        }
    }
    if (revisions.skin != m_localShadowRevisions.skin) {
        for (const RenderItem& item : m_renderItems) {
            if (!item.jointPalette.empty()) {
                addCaster(item);
            }
        }
    }
}

bool MeshPass::drawLocalShadowCasters(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                      const glm::mat4& viewProjection, RenderStats& stats) {
    const std::array<glm::vec4, 6> frustum = clipVolumePlanes(viewProjection);
    m_shadowVisibleItems.clear();
    for (const RenderItem& item : m_renderItems) {
        if (item.material->alphaMode != MaterialAlphaMode::Blend &&
            sphereReachesClipVolume(frustum, item.boundsCenter, item.boundsRadius)) {
            m_shadowVisibleItems.push_back(&item);
        }
    }
    if (m_localShadowObjectCount + m_shadowVisibleItems.size() > maximumObjectsPerFrame) {
        return false;
    }
    // Keep each primitive's LOD range adjacent so its instances share a draw.
    std::ranges::stable_sort(m_shadowVisibleItems, {}, [](const RenderItem* item) {
        return std::tuple(item->material->doubleSided, handleKey(item->meshHandle), item->lod.firstIndex);
    });
    const uint32_t objectBase = localShadowObjectBase + m_localShadowObjectCount;
    auto* objectData = static_cast<ObjectData*>(m_cameras[frameIndex].objectsMapped);
    for (size_t index = 0; index < m_shadowVisibleItems.size(); ++index) {
        const RenderItem& item = *m_shadowVisibleItems[index];
        objectData[objectBase + index] = {.model = item.world,
                                          .previousModel = item.previousWorld,
                                          .skin = {item.skinPaletteOffset,
                                                   static_cast<uint32_t>(item.jointPalette.size()),
                                                   item.gpuMaterial->index, 0}};
    }
    if (!m_shadowVisibleItems.empty()) {
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].objects.allocation, sizeof(ObjectData) * objectBase,
                           sizeof(ObjectData) * m_shadowVisibleItems.size());
    }
    m_localShadowObjectCount += static_cast<uint32_t>(m_shadowVisibleItems.size());
    std::optional<uint32_t> boundPage;
    std::optional<bool> boundDoubleSided;
    for (size_t itemIndex = 0; itemIndex < m_shadowVisibleItems.size();) {
        const RenderItem& item = *m_shadowVisibleItems[itemIndex];
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < m_shadowVisibleItems.size()) {
            const RenderItem& candidate = *m_shadowVisibleItems[batchEnd];
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        if (!boundDoubleSided || *boundDoubleSided != item.material->doubleSided) {
            vkCmdSetCullMode(commandBuffer, item.material->doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            boundDoubleSided = item.material->doubleSided;
        }
        bindGeometry(commandBuffer, *item.gpuMesh, boundPage);
        const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
        vkCmdDrawIndexed(commandBuffer, item.lod.indexCount, instanceCount,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         objectBase + static_cast<uint32_t>(itemIndex));
        stats.localShadowCasterCount += instanceCount;
        ++stats.drawCalls;
        stats.triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
        itemIndex = batchEnd;
    }
    return true;
}

bool MeshPass::recordShadow(VkCommandBuffer commandBuffer, uint32_t frameIndex, const std::vector<RenderItem>& items,
                            const DirectionalLightComponent& light,
                            const std::array<glm::mat4, maximumShadowCascades>& lightViewProjections,
                            uint32_t cascadeCount, const std::array<bool, maximumShadowCascades>& dirtyCascades,
                            ShadowCulling culling, const MeshLodView& lodView, OptionalPassTimestamps& passTimestamps,
                            RenderStats& stats) {
    if (cascadeCount == 0 || std::ranges::none_of(dirtyCascades, [](bool dirty) { return dirty; })) {
        stats.shadowCascadesCached = cascadeCount;
        return true;
    }
    // Disabled light shadows still clear the dirty cascades, without casters.
    const bool drawCasters = light.shadows.enabled;
    if (culling == ShadowCulling::Gpu && drawCasters) {
        // All dirty cascades are culled up front: compute work cannot run
        // inside the cascades' rendering scopes.
        passTimestamps.write(FrameTimestamp::shadowCullBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
            if (dirtyCascades[cascade]) {
                m_gpuDrivenPass.cullFrustumView(commandBuffer, frameIndex, cascade, lightViewProjections[cascade],
                                                lodView);
            }
        }
        passTimestamps.write(FrameTimestamp::shadowCullEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    imageBarrier(commandBuffer, m_shadowTexture.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                 m_shadowInitialized ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 // Earlier frames sampled it in shading and volumetric fog.
                 m_shadowInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 m_shadowInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, maximumShadowCascades);
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = VK_NULL_HANDLE,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {.width = shadowResolution, .height = shadowResolution}},
        .layerCount = 1,
        .pDepthAttachment = &depth};
    VkViewport viewport{.x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<float>(shadowResolution),
                        .height = static_cast<float>(shadowResolution),
                        .minDepth = 0.0f,
                        .maxDepth = 1.0f};
    VkRect2D scissor{.offset = {0, 0}, .extent = {.width = shadowResolution, .height = shadowResolution}};
    for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
        if (!dirtyCascades[cascade]) {
            ++stats.shadowCascadesCached;
            continue;
        }
        ++stats.shadowCascadesRendered;
        depth.imageView = m_shadowLayerViews[cascade];
        vkCmdBeginRendering(commandBuffer, &rendering);
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.handle());
        vkCmdSetDepthBias(commandBuffer, std::max(light.shadows.constantBias, 0.0f), 0.0f,
                          std::max(light.shadows.slopeBias, 0.0f));
        const glm::vec4 shadowPush{static_cast<float>(cascade), 0.0f, 0.0f, 0.0f};
        vkCmdPushConstants(commandBuffer, m_shadowPipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shadowPush),
                           &shadowPush);
        if (culling == ShadowCulling::Gpu) {
            if (drawCasters) {
                recordGpuShadowCascade(commandBuffer, frameIndex, cascade, stats);
            }
            vkCmdEndRendering(commandBuffer);
            continue;
        }
        const VkDescriptorSet shadowSets[]{m_cameras[frameIndex].descriptor, m_cameras[frameIndex].bindlessDescriptor};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.layout(), 0,
                                static_cast<uint32_t>(std::size(shadowSets)), shadowSets, 0, nullptr);
        const std::array<glm::vec4, 6> shadowFrustum = clipVolumePlanes(lightViewProjections[cascade]);
        m_shadowVisibleItems.clear();
        m_shadowVisibleItems.reserve(items.size());
        for (const RenderItem& item : items) {
            if (light.shadows.enabled && item.material->alphaMode != MaterialAlphaMode::Blend &&
                sphereReachesClipVolume(shadowFrustum, item.boundsCenter, item.boundsRadius)) {
                m_shadowVisibleItems.push_back(&item);
            }
        }
        // Instances of one primitive may select different LODs; keep each
        // range adjacent so its instances still share a draw.
        std::ranges::stable_sort(m_shadowVisibleItems, {}, [](const RenderItem* item) {
            return std::tuple(item->material->doubleSided, handleKey(item->meshHandle), item->lod.firstIndex);
        });
        const uint32_t objectBase = cascade * maximumObjectsPerFrame;
        auto* objectData = static_cast<ObjectData*>(m_cameras[frameIndex].objectsMapped);
        for (size_t index = 0; index < m_shadowVisibleItems.size(); ++index) {
            objectData[objectBase + index].model = m_shadowVisibleItems[index]->world;
            objectData[objectBase + index].previousModel = m_shadowVisibleItems[index]->previousWorld;
            objectData[objectBase + index].skin = {
                m_shadowVisibleItems[index]->skinPaletteOffset,
                static_cast<uint32_t>(m_shadowVisibleItems[index]->jointPalette.size()),
                m_shadowVisibleItems[index]->gpuMaterial->index, 0};
        }
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].objects.allocation, 0, VK_WHOLE_SIZE);
        std::optional<uint32_t> boundPage;
        std::optional<bool> boundDoubleSided;
        for (size_t itemIndex = 0; itemIndex < m_shadowVisibleItems.size();) {
            const RenderItem& item = *m_shadowVisibleItems[itemIndex];
            size_t batchEnd = itemIndex + 1;
            while (batchEnd < m_shadowVisibleItems.size()) {
                const RenderItem& candidate = *m_shadowVisibleItems[batchEnd];
                // Materials are per instance now, so only geometry and culling
                // state split a shadow batch.
                if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                    candidate.lod.indexCount != item.lod.indexCount ||
                    candidate.material->doubleSided != item.material->doubleSided) {
                    break;
                }
                ++batchEnd;
            }
            if (!boundDoubleSided || *boundDoubleSided != item.material->doubleSided) {
                vkCmdSetCullMode(commandBuffer, item.material->doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
                boundDoubleSided = item.material->doubleSided;
            }
            bindGeometry(commandBuffer, *item.gpuMesh, boundPage);
            const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
            vkCmdDrawIndexed(commandBuffer, item.lod.indexCount, instanceCount,
                             item.gpuMesh->geometry.firstIndex + item.lod.firstIndex,
                             item.gpuMesh->geometry.vertexOffset, objectBase + static_cast<uint32_t>(itemIndex));
            stats.shadowCasterCount += instanceCount;
            ++stats.drawCalls;
            stats.triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
            itemIndex = batchEnd;
        }
        vkCmdEndRendering(commandBuffer);
    }
    imageBarrier(
        commandBuffer, m_shadowTexture.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, maximumShadowCascades);
    m_shadowInitialized = true;
    return true;
}

void MeshPass::recordGpuShadowCascade(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t cascade,
                                      RenderStats& stats) {
    // The GPU-driven set binds the cull output as the object buffer, so
    // shadow.vert reads each instance by gl_InstanceIndex on both paths.
    const VkDescriptorSet shadowSets[]{m_cameras[frameIndex].gpuDrivenDescriptor,
                                       m_cameras[frameIndex].bindlessDescriptor};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.layout(), 0,
                            static_cast<uint32_t>(std::size(shadowSets)), shadowSets, 0, nullptr);
    std::optional<bool> boundDoubleSided;
    for (uint32_t group = 0; group < m_drawPlan.groups.size(); ++group) {
        const IndirectDrawGroup& drawGroup = m_drawPlan.groups[group];
        const bool doubleSided = drawGroup.pipeline == doubleSidedBucket;
        if (!boundDoubleSided || *boundDoubleSided != doubleSided) {
            vkCmdSetCullMode(commandBuffer, doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            boundDoubleSided = doubleSided;
        }
        bindGeometryPage(commandBuffer, drawGroup.geometryPage);
        m_gpuDrivenPass.drawGroup(commandBuffer, GpuCullRegion::frustumView(cascade), group, drawGroup.firstBatch,
                                  drawGroup.batchCount);
        ++stats.drawCalls;
    }
}

bool MeshPass::record(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                      VkImageView depthView, VkExtent2D extent, uint32_t frameIndex, const Scene& scene,
                      const AssetManager& assets, RenderRuntime& rendering, FrameTaskExecutor& frameTasks,
                      VkQueryPool timestampQueries, VkImageLayout colorOldLayout) {
    if (frameIndex >= m_cameras.size()) {
        return false;
    }

    const double previousGpuFrameMilliseconds = rendering.m_stats.gpuFrameMilliseconds;
    rendering.m_stats = {};

    // The caller waited for this frame slot's fence, so every frame at least
    // one full ring older than this one has completed on the GPU.
    ++m_frameSerial;
    if (m_frameSerial > m_cameras.size()) {
        m_releaseQueue.collect(m_frameSerial - m_cameras.size());
        m_volumetricFog.collect(m_frameSerial - m_cameras.size());
    }
    pruneUnusedAssets(assets);

    glm::mat4 view{1.0f};
    float fov = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    auto cameras = scene.registry().view<const CameraComponent, const TransformComponent>();

    for (Entity entity : cameras) {
        const auto& camera = cameras.get<const CameraComponent>(entity);

        if (camera.primary) {
            view = glm::inverse(cameras.get<const TransformComponent>(entity).world);
            fov = camera.verticalFovRadians;
            nearPlane = camera.nearPlane;
            farPlane = camera.farPlane;
            break;
        }
    }
    if (rendering.m_viewOverride) {
        const RenderView& override = *rendering.m_viewOverride;
        view = glm::lookAtRH(override.position, override.position + override.forward, override.up);
        fov = override.verticalFovRadians;
        nearPlane = override.nearPlane;
        farPlane = override.farPlane;
    }
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(view)[3]);
    const glm::vec3 cameraForward = -glm::normalize(glm::vec3(glm::inverse(view)[2]));
    const bool cameraCut = !m_cameraHistoryValid || glm::distance(cameraPosition, m_previousCameraPosition) > 10.0f ||
                           glm::dot(cameraForward, m_previousCameraForward) < 0.5f;
    const TemporalFrameState temporal = m_temporalQuality.beginFrame(
        extent.width, extent.height, previousGpuFrameMilliseconds, rendering.settings().dynamicResolution,
        rendering.settings().antialiasing == AntialiasingMode::Taa, cameraCut);
    const VkExtent2D renderExtent{temporal.internalWidth, temporal.internalHeight};
    // Swapping the planes yields reversed Z; see DepthConvention.hpp.
    glm::mat4 projection = glm::perspectiveRH_ZO(
        fov, static_cast<float>(renderExtent.width) / static_cast<float>(renderExtent.height), farPlane, nearPlane);
    projection[1][1] *= -1.0f;
    const glm::mat4 unjitteredViewProjection = projection * view;
    if (rendering.settings().antialiasing == AntialiasingMode::Taa) {
        projection = applyProjectionJitter(projection, temporal.jitterNdc);
    }
    const DirectionalLightComponent light = resolvePrimary<DirectionalLightComponent>(scene);
    const EnvironmentLightComponent environment = resolvePrimary<EnvironmentLightComponent>(scene);
    const std::optional<VolumetricFogComponent> fogMedium = primaryVolumetricFog(scene);
    // Hemisphere lighting reads these flat colors; image-based lighting reads
    // the maps EnvironmentPass binds.
    std::array<glm::vec3, 6> environmentSpecular;
    environmentSpecular.fill((environment.skyColor + environment.groundColor) * 0.5f);
    const glm::vec3 lightDirection = normalizedLightDirection(light.direction);
    CameraData cameraData;
    cameraData.viewProjection = projection * view;
    cameraData.previousViewProjection = m_previousViewProjection;
    cameraData.temporalJitter = {temporal.jitterNdc, m_previousJitterNdc};
    const float shadowDepthRange = std::max(light.shadows.depthRange, 0.2f);
    uint32_t cascadeCount = 0;
    switch (rendering.settings().shadows) {
    case ShadowQuality::Off:
        break;
    case ShadowQuality::Single:
        cascadeCount = 1;
        break;
    case ShadowQuality::CascadedBalanced:
        cascadeCount = 2;
        break;
    case ShadowQuality::CascadedHigh:
        cascadeCount = 3;
        break;
    }
    const bool depthPrepass =
        rendering.settings().depthPrepass && SDL_getenv("ZENITH_DISABLE_DEPTH_PREPASS") == nullptr;
    const MeshPipelineSet* shadingPipelines =
        meshPipelines({.physicallyBased = rendering.settings().lighting == LightingQuality::PhysicallyBased,
                       .shadowCascades = cascadeCount,
                       .depthPrepass = depthPrepass});
    if (!shadingPipelines) {
        return false;
    }
    const float maximumShadowDistance = std::max(std::min(farPlane, shadowDepthRange), nearPlane + 0.01f);
    const glm::vec3 lightUp =
        std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    float cascadeNear = nearPlane;
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    for (uint32_t cascade = 0; cascade < std::max(cascadeCount, 1u); ++cascade) {
        float cascadeFar = maximumShadowDistance;
        float radius = std::max(light.shadows.halfExtent, 0.1f);
        glm::vec3 center = cameraPosition + cameraForward * std::max(light.shadows.focusDistance, 0.0f);
        if (cascadeCount > 1) {
            const float partition = static_cast<float>(cascade + 1) / static_cast<float>(cascadeCount);
            const float logarithmic = nearPlane * std::pow(maximumShadowDistance / nearPlane, partition);
            const float uniform = nearPlane + (maximumShadowDistance - nearPlane) * partition;
            const float lambda = std::clamp(rendering.settings().cascadeSplitLambda, 0.0f, 1.0f);
            cascadeFar = glm::mix(uniform, logarithmic, lambda);
            const float halfHeight = std::tan(fov * 0.5f) * cascadeFar;
            const float halfWidth = halfHeight * aspect;
            radius = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight +
                               (cascadeFar - cascadeNear) * (cascadeFar - cascadeNear) * 0.25f);
            radius = std::ceil(radius * 16.0f) / 16.0f;
            center = cameraPosition + cameraForward * ((cascadeNear + cascadeFar) * 0.5f);
        }
        cameraData.cascadeSplits[cascade] = cascadeFar;
        const glm::mat4 lightView = glm::lookAtRH(center + lightDirection * shadowDepthRange * 0.5f, center, lightUp);
        glm::mat4 lightProjection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, shadowDepthRange * 1.5f);
        lightProjection[1][1] *= -1.0f;
        glm::vec4 shadowOrigin = lightProjection * lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        shadowOrigin *= static_cast<float>(shadowResolution) * 0.5f;
        glm::vec4 roundingOffset =
            (glm::round(shadowOrigin) - shadowOrigin) * (2.0f / static_cast<float>(shadowResolution));
        roundingOffset.z = 0.0f;
        roundingOffset.w = 0.0f;
        lightProjection[3] += roundingOffset;
        cameraData.lightViewProjections[cascade] = lightProjection * lightView;
        cascadeNear = cascadeFar;
    }
    cameraData.cameraPosition = {cameraPosition, 1.0f};
    cameraData.lightDirection = {lightDirection, std::max(light.intensity, 0.0f)};
    cameraData.lightColor = {glm::max(light.color, glm::vec3(0.0f)), 1.0f};
    cameraData.environmentSky = {glm::max(environment.skyColor, glm::vec3(0.0f)),
                                 std::max(environment.diffuseIntensity, 0.0f)};
    cameraData.environmentGround = {glm::max(environment.groundColor, glm::vec3(0.0f)),
                                    std::max(environment.specularIntensity, 0.0f)};
    const bool shadowsEnabled = light.shadows.enabled && rendering.settings().shadows != ShadowQuality::Off;
    cameraData.shadowSettings = {std::max(light.shadows.normalBias, 0.0f), shadowsEnabled ? 1.0f : 0.0f,
                                 static_cast<float>(cascadeCount),
                                 std::clamp(rendering.settings().cascadeBlendFraction, 0.0f, 0.5f)};
    cameraData.renderingSettings = {
        rendering.settings().lighting == LightingQuality::PhysicallyBased ? 1.0f : 0.0f,
        1.0f / static_cast<float>(extent.width),
        1.0f / static_cast<float>(extent.height),
        rendering.settings().visualizeShadowCascades ? 1.0f : 0.0f,
    };
    for (size_t level = 0; level < environmentSpecular.size(); ++level) {
        cameraData.environmentSpecular[level] = {glm::max(environmentSpecular[level], glm::vec3(0.0f)), 1.0f};
    }
    // Environment maps are bound before any pass uses this frame's camera sets.
    const bool imageBasedLighting = rendering.settings().environment != EnvironmentQuality::Hemisphere;
    const CameraBuffer& frameCamera = m_cameras[frameIndex];
    const std::array<VkDescriptorSet, 2> environmentSets{frameCamera.descriptor, frameCamera.gpuDrivenDescriptor};
    std::vector<EnvironmentSource> residentProbeSources;
    const std::vector<ReflectionProbeVolume> residentProbes =
        perPixelReflectionProbes(rendering.settings())
            ? selectPerPixelProbes(scene, assets, environment, cameraPosition, residentProbeSources)
            : std::vector<ReflectionProbeVolume>{};
    OptionalPassTimestamps passTimestamps(commandBuffer, timestampQueries);
    passTimestamps.write(FrameTimestamp::environmentPrecomputeBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const std::optional<EnvironmentFrameState> environmentState =
        m_environmentPass.prepare(commandBuffer, frameIndex, m_frameSerial,
                                  environmentRequest(scene, assets, environment, rendering.settings(), cameraPosition,
                                                     std::move(residentProbeSources)),
                                  std::span(environmentSets.data(), frameCamera.gpuDrivenDescriptor ? 2u : 1u));
    passTimestamps.write(FrameTimestamp::environmentPrecomputeEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (!environmentState) {
        return false;
    }
    // Probes still waiting for their maps to build are left out this frame.
    std::vector<GpuReflectionProbe> probeRecords;
    for (size_t index = 0; index < residentProbes.size() && index < environmentState->probeSlots.size(); ++index) {
        if (const int32_t slot = environmentState->probeSlots[index]; slot >= 0) {
            probeRecords.push_back(packReflectionProbe(residentProbes[index], static_cast<uint32_t>(slot), view));
        }
    }
    cameraData.reflectionProbeParams = {probeRecords.empty() ? 0.0f : 1.0f, static_cast<float>(probeRecords.size()),
                                        0.0f, 0.0f};
    // GTAO needs the prepass normals; without them the occlusion target is
    // cleared to unoccluded and any scale samples it correctly.
    const AmbientOcclusionQuality ambientOcclusion =
        depthPrepass ? rendering.settings().ambientOcclusion : AmbientOcclusionQuality::Off;
    cameraData.environmentParams = {imageBasedLighting ? 1.0f : 0.0f, environmentState->probeWeight,
                                    environmentState->specularMaximumLod,
                                    AmbientOcclusionPass::resolutionScale(ambientOcclusion)};
    // Irradiance volume (U6): bind the volume nearest the camera and upload its
    // probes when they change; Environment mode leaves the volume disabled.
    passTimestamps.write(FrameTimestamp::irradianceVolumeBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const Entity irradianceVolumeEntity = rendering.settings().indirectDiffuse == IndirectDiffuseMode::ProbeVolume
                                              ? irradiance_volume::selectVolume(scene, cameraPosition)
                                              : entt::null;
    const IrradianceVolumeComponent* irradianceVolume =
        irradianceVolumeEntity == entt::null ? nullptr
                                             : &scene.registry().get<IrradianceVolumeComponent>(irradianceVolumeEntity);
    if (!m_irradianceVolume.prepare(commandBuffer, frameIndex, m_frameSerial,
                                    irradianceVolume ? irradianceVolume->bakedData : nullptr,
                                    std::span(environmentSets.data(), frameCamera.gpuDrivenDescriptor ? 2u : 1u))) {
        return false;
    }
    passTimestamps.write(FrameTimestamp::irradianceVolumeEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (irradianceVolume) {
        const IrradianceVolumeData& volumeData = *irradianceVolume->bakedData;
        cameraData.irradianceVolumeMinimum = {volumeData.minimum, std::max(irradianceVolume->intensity, 0.0f)};
        cameraData.irradianceVolumeCellSize = {volumeData.cellSize(), std::max(irradianceVolume->blendDistance, 0.0f)};
        cameraData.irradianceVolumeCounts = {volumeData.probeCounts, 1u};
        cameraData.irradianceVolumeBias = {std::max(irradianceVolume->normalBias, 0.0f),
                                           std::max(irradianceVolume->viewBias, 0.0f), 0.0f, 0.0f};
    }
    const uint32_t localLightLimit =
        rendering.settings().localLights == LocalLightingQuality::Disabled
            ? 0u
            : std::min(rendering.settings().maximumLocalLights, m_clusteredLighting.lightCapacity());
    std::vector<LocalLight> selectedLights;
    size_t sceneLightCount = 0;

    uint64_t outlineRevision = 0;
    for (RenderObjectId outlined : rendering.m_outlinedObjects) {
        outlineRevision ^=
            static_cast<uint64_t>(outlined) + 0x9e3779b97f4a7c15ull + (outlineRevision << 6) + (outlineRevision >> 2);
    }
    const SceneRevisions& revisions = scene.revisions();
    const bool rebuildPackets = !m_packetCachingEnabled || m_meshUploadsPending ||
                                revisions.renderable != m_sceneRevisions.renderable ||
                                revisions.material != m_sceneRevisions.material ||
                                assets.revision() != m_assetRevision || outlineRevision != m_outlineRevision;
    const auto packetBuildStart = std::chrono::steady_clock::now();
    uint32_t scannedRenderables = 0;
    uint32_t updatedObjectRecords = 0;
    uint32_t allocationCount = 0;
    if (rebuildPackets) {
        m_renderItems.clear();
        const size_t previousCapacity = m_renderItems.capacity();
        if (!buildRenderItems(scene, assets, rendering.m_outlinedObjects, m_renderItems)) {
            return false;
        }
        scannedRenderables = static_cast<uint32_t>(scene.registry().view<const MeshRendererComponent>().size());
        updatedObjectRecords = static_cast<uint32_t>(m_renderItems.size());
        allocationCount = m_renderItems.capacity() != previousCapacity ? 1u : 0u;
    } else if (revisions.transform != m_sceneRevisions.transform || revisions.skin != m_sceneRevisions.skin) {
        updatedObjectRecords = updateRenderItems(scene, scene.changedTransforms());
        if (revisions.skin != m_sceneRevisions.skin) {
            const auto skins = scene.registry().view<const SkinComponent>();
            for (Entity entity : skins) {
                updatedObjectRecords += updateRenderItems(scene, std::span<const Entity>(&entity, 1));
            }
        }
    }
    m_sceneRevisions = revisions;
    m_assetRevision = assets.revision();
    m_outlineRevision = outlineRevision;
    writeMaterialRecords(frameIndex, assets);
    updateBindlessDescriptor(frameIndex);
    const auto packetBuildEnd = std::chrono::steady_clock::now();
    const double packetBuildMilliseconds =
        std::chrono::duration<double, std::milli>(packetBuildEnd - packetBuildStart).count();
    if (m_renderItems.size() > maximumObjectsPerFrame) {
        std::cerr << "Renderable count exceeds the per-frame render capacity\n";
        return false;
    }
    // Opaque and masked surfaces go through GPU culling; blended surfaces and
    // editor outlines keep CPU submission within the same frame.
    const bool gpuDrivenRequested =
        rendering.settings().preferGpuDrivenSubmission || SDL_getenv("ZENITH_FORCE_GPU_DRIVEN") != nullptr;
    const bool gpuDrivenActive = m_gpuDrivenSupported && gpuDrivenRequested && rendering.capabilities().gpuDrivenPath &&
                                 rendering.capabilities().descriptorIndexing &&
                                 rendering.capabilities().multiDrawIndirect;
    const FrameResourceUse lightResources[]{{"scene.lights", FrameResourceAccess::Read},
                                            {"frame.light_list", FrameResourceAccess::Write}};
    const FrameResourceUse visibilityResources[]{{"renderer.persistent_scene", FrameResourceAccess::Read},
                                                 {"frame.visibility", FrameResourceAccess::Write}};
    FrameTaskGraph taskGraph(++m_taskGeneration);
    taskGraph.add("light-selection", lightResources, [&](const FrameTaskContext&) {
        if (localLightLimit > 0) {
            const std::vector<LocalLight> sceneLights = gatherLocalLights(scene);
            sceneLightCount = sceneLights.size();
            selectedLights = selectLocalLights(sceneLights, cameraPosition, localLightLimit);
        }
    });
    taskGraph.add("render-visibility", visibilityResources, [&](const FrameTaskContext&) {
        m_gpuScene.buildVisibility({.viewProjection = cameraData.viewProjection, .depthHistoryValid = false},
                                   m_visibility);
    });
    const FrameTaskResult taskResult = frameTasks.execute(taskGraph);
    if (!taskResult) {
        std::cerr << "Frame task graph failed: " << taskResult.error << '\n';
        return false;
    }
    m_gpuScene.retireDeltas();
    ClusterLightStatistics clusterStatistics;
    const uint32_t localLightCount = m_clusteredLighting.beginFrame(frameIndex, selectedLights, clusterStatistics);
    const ClusterGrid clusterGrid = makeClusterGrid(m_clusteredLighting.configuration(), projection,
                                                    {renderExtent.width, renderExtent.height}, nearPlane, farPlane);
    const glm::vec2 sliceScaleBias = clusterGrid.sliceScaleBias();
    cameraData.clusterDepth = {sliceScaleBias, projection[3][2], projection[2][2]};
    cameraData.clusterTileScale = {glm::vec2(clusterGrid.configuration.columns, clusterGrid.configuration.rows) /
                                       clusterGrid.renderExtent,
                                   0.0f, 0.0f};
    cameraData.clusterGrid = {clusterGrid.configuration.columns, clusterGrid.configuration.rows,
                              clusterGrid.configuration.depthSlices, localLightCount};
    // Fog volumes follow the quality setting; the scene decides whether there
    // is a medium to draw at all.
    if (!m_volumetricFog.prepare(rendering.settings().volumetricFog, m_frameSerial)) {
        return false;
    }
    writeVolumetricFogDescriptors(frameIndex);
    const bool fogActive = rendering.settings().volumetricFog != VolumetricFogQuality::Off &&
                           m_volumetricFog.supported() && fogMedium && fogMedium->density > 0.0f;
    const volumetric_fog::FroxelGrid froxelGrid =
        volumetric_fog::makeFroxelGrid(clusterGrid, m_volumetricFog.subdivision());
    if (fogActive) {
        cameraData.fogVolume = {1.0f, froxelGrid.sliceScaleBias(), static_cast<float>(froxelGrid.dimensions().z)};
        cameraData.fogScreen = {1.0f / static_cast<float>(renderExtent.width),
                                1.0f / static_cast<float>(renderExtent.height), 0.0f, 0.0f};
    }
    std::memcpy(m_cameras[frameIndex].mapped, &cameraData, sizeof(cameraData));
    vmaFlushAllocation(m_allocator, m_cameras[frameIndex].buffer.allocation, 0, sizeof(cameraData));
    RenderStats& stats = rendering.m_stats;
    stats.cpuRenderPacketMilliseconds = packetBuildMilliseconds;
    stats.cpuUploadMilliseconds = m_uploadMilliseconds;
    stats.gpuUploadMilliseconds = m_uploadGpuMilliseconds;
    stats.uploadBytes = m_uploadContext.takeStagedBytes() + m_textureUploadContext.takeStagedBytes();
    stats.uploadBackpressureEvents =
        m_uploadContext.takeBackpressureEvents() + m_textureUploadContext.takeBackpressureEvents();
    stats.uploadTimingsValid =
        stats.uploadBytes != 0 && (m_uploadContext.timingsSupported() || m_textureUploadContext.timingsSupported());
    m_uploadMilliseconds = 0.0;
    m_uploadGpuMilliseconds = 0.0;
    stats.renderPacketsRebuilt = rebuildPackets;
    stats.scannedRenderables = scannedRenderables;
    stats.updatedObjectRecords = updatedObjectRecords;
    stats.mainThreadAllocations = allocationCount;
    stats.localLightCount = localLightCount;
    stats.reflectionProbeCount = static_cast<uint32_t>(probeRecords.size());
    stats.discardedLightAssignments = clusterStatistics.discardedAssignments;
    stats.maximumLightsPerCluster = clusterStatistics.maximumLightsInCluster;
    stats.droppedLocalLights =
        sceneLightCount > localLightCount ? static_cast<uint32_t>(sceneLightCount - localLightCount) : 0u;
    recordDeferredUploads(rendering.settings(), stats);
    stats.taskQueueDepth = taskResult.stats.maximumQueueDepth;
    stats.taskStolenJobs = taskResult.stats.stolenJobs;
    stats.taskWorkerUtilization = taskResult.stats.workerUtilization;
    stats.taskMainThreadAssistMilliseconds = taskResult.stats.mainThreadAssistMilliseconds;
    stats.taskGraphLatencyMilliseconds = taskResult.stats.latencyMilliseconds;
    stats.renderableCount = static_cast<uint32_t>(m_renderItems.size());
    stats.internalRenderWidth = renderExtent.width;
    stats.internalRenderHeight = renderExtent.height;
    stats.internalResolutionScale = temporal.resolutionScale;
    stats.temporalHistoryValid = temporal.historyValid;
    stats.temporalHistoryInvalidated = temporal.historyInvalidated;
    stats.temporalHistoryBytes = m_postProcess.temporalHistoryBytes();
    stats.motionVectorBytes =
        static_cast<uint64_t>(extent.width) * extent.height * sizeof(uint16_t) * 4 * m_cameras.size();
    stats.ambientOcclusionBytes = m_ambientOcclusion.allocatedBytes();
    stats.volumetricFogBytes = m_volumetricFog.allocatedBytes();
    stats.irradianceVolumeBytes = m_irradianceVolume.allocatedBytes();
    stats.irradianceVolumeProbes =
        irradianceVolume ? static_cast<uint32_t>(irradianceVolume->bakedData->probes.size()) : 0u;
    stats.shadowCacheBytes =
        static_cast<uint64_t>(shadowResolution) * shadowResolution * sizeof(float) * maximumShadowCascades;
    DirectionalLightComponent shadowLight = light;
    shadowLight.shadows.enabled = shadowsEnabled;
    auto* skinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].skinPalettesMapped);
    // Last frame's palettes at the same offsets, so skinned motion vectors see
    // joint animation as well as object motion.
    auto* previousSkinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].previousSkinPalettesMapped);
    uint32_t skinMatrixCount = 1;
    skinMatrices[0] = glm::mat4(1.0f);
    previousSkinMatrices[0] = glm::mat4(1.0f);
    for (RenderItem& item : m_renderItems) {
        item.skinPaletteOffset = 0;
        if (item.jointPalette.empty()) {
            continue;
        }
        if (skinMatrixCount + item.jointPalette.size() > maximumSkinMatricesPerFrame) {
            std::cerr << "Skin palettes exceed the per-frame render capacity\n";
            return false;
        }
        item.skinPaletteOffset = skinMatrixCount;
        std::copy(item.jointPalette.begin(), item.jointPalette.end(), skinMatrices + skinMatrixCount);
        const std::vector<glm::mat4>& previousPalette = item.previousJointPalette.size() == item.jointPalette.size()
                                                            ? item.previousJointPalette
                                                            : item.jointPalette;
        std::copy(previousPalette.begin(), previousPalette.end(), previousSkinMatrices + skinMatrixCount);
        skinMatrixCount += static_cast<uint32_t>(item.jointPalette.size());
    }
    vmaFlushAllocation(m_allocator, m_cameras[frameIndex].skinPalettes.allocation, 0,
                       sizeof(glm::mat4) * skinMatrixCount);
    vmaFlushAllocation(m_allocator, m_cameras[frameIndex].previousSkinPalettes.allocation, 0,
                       sizeof(glm::mat4) * skinMatrixCount);
    if (timestampQueries) {
        // The transfer timeline wait is scoped to vertex/index consumption, so
        // this timestamp exposes any queue bubble before the first consumer.
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, timestampQueries, 16);
    }
    const MeshLodView lodView = makeMeshLodView(cameraPosition, projection[1][1], renderExtent.height,
                                                rendering.settings().lodErrorThresholdPixels);
    selectRenderItemLods(lodView);
    // Local shadows may rebind the atlas in the camera sets, so they run before
    // GPU-driven preparation and before anything binds those sets.
    recordLocalShadows(commandBuffer, frameIndex, scene, assets, rendering.settings(),
                       std::span<const LocalLight>(selectedLights.data(), localLightCount), cameraPosition,
                       passTimestamps, stats);
    if (gpuDrivenActive) {
        // Shadow cascades cull from the same records before the camera does.
        if (!prepareGpuDrivenFrame(frameIndex, lodView)) {
            return false;
        }
        m_gpuDrivenPass.beginFrame(commandBuffer, frameIndex);
        // Every cull dispatch statically references the pyramid.
        m_depthPyramid.prepareFirstUse(commandBuffer);
    }
    // Cascades cull on the GPU whenever GPU-driven submission is active; the CPU
    // path is the fallback and the A/B baseline (docs/RENDERER_VALIDATION.md).
    const ShadowCulling shadowCulling = gpuDrivenActive && SDL_getenv("ZENITH_DISABLE_GPU_SHADOW_CULLING") == nullptr
                                            ? ShadowCulling::Gpu
                                            : ShadowCulling::Cpu;
    const auto shadowCpuBegin = std::chrono::steady_clock::now();
    // ZENITH_DISABLE_SHADOW_CACHE redraws every cascade each frame, so shadow
    // rendering and culling can be measured on a static scene.
    const bool shadowCastersChanged =
        !m_shadowInitialized || SDL_getenv("ZENITH_DISABLE_SHADOW_CACHE") != nullptr ||
        revisions.transform != m_shadowRevisions.transform || revisions.renderable != m_shadowRevisions.renderable ||
        revisions.light != m_shadowRevisions.light || revisions.skin != m_shadowRevisions.skin ||
        revisions.material != m_shadowRevisions.material;
    std::array<bool, maximumShadowCascades> dirtyCascades{};
    for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
        float maximumDifference = 0.0f;
        for (uint32_t column = 0; column < 4; ++column) {
            for (uint32_t row = 0; row < 4; ++row) {
                maximumDifference =
                    std::max(maximumDifference, std::abs(cameraData.lightViewProjections[cascade][column][row] -
                                                         m_cachedLightViewProjections[cascade][column][row]));
            }
        }
        dirtyCascades[cascade] = shadowCastersChanged || cascade >= m_cachedCascadeCount || maximumDifference > 0.0001f;
    }
    if (!recordShadow(commandBuffer, frameIndex, m_renderItems, shadowLight, cameraData.lightViewProjections,
                      cascadeCount, dirtyCascades, shadowCulling, lodView, passTimestamps, stats)) {
        return false;
    }
    m_shadowRevisions = revisions;
    m_cachedLightViewProjections = cameraData.lightViewProjections;
    m_cachedCascadeCount = cascadeCount;
    stats.cpuShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowCpuBegin).count();
    const DrawContext draw{.commandBuffer = commandBuffer,
                           .frameIndex = frameIndex,
                           .pipelines = shadingPipelines,
                           .timestampQueries = timestampQueries,
                           .passTimestamps = &passTimestamps,
                           .stats = &stats,
                           .view = view,
                           .renderExtent = renderExtent};
    if (gpuDrivenActive) {
        // Culling time covers the early phase; the depth pyramid and late phase
        // run between the opaque passes and count toward opaque time.
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 8);
        }
        m_gpuDrivenPass.cull(commandBuffer, frameIndex, GpuCullPhase::Early, cameraData.viewProjection, m_depthPyramid,
                             lodView);
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 9);
        }
    } else if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 8);
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 9);
    }
    m_previousCameraPosition = cameraPosition;
    m_previousCameraForward = cameraForward;
    m_cameraHistoryValid = true;
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 1);
    }
    if (!collectCpuSubmittedItems(gpuDrivenActive, cameraPosition)) {
        return false;
    }
    uploadCpuSubmittedObjects(frameIndex);
    stats.culledCount += m_visibility.frustumCulled + (gpuDrivenActive ? 0u : m_visibility.occlusionCulled);

    passTimestamps.write(FrameTimestamp::lightClustersBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    m_clusteredLighting.record(commandBuffer, frameIndex, clusterGrid, view);
    passTimestamps.write(FrameTimestamp::lightClustersEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (!probeRecords.empty()) {
        passTimestamps.write(FrameTimestamp::reflectionProbeClustersBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        m_reflectionProbeClusters.record(commandBuffer, frameIndex, probeRecords,
                                         m_environmentPass.probeIrradianceBuffer(), clusterGrid);
        passTimestamps.write(FrameTimestamp::reflectionProbeClustersEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    if (fogActive) {
        const ClusteredLightingPass::FrameBuffers clusterBuffers = m_clusteredLighting.frameBuffers(frameIndex);
        const VolumetricFogFrame fogFrame{
            .grid = froxelGrid,
            .inverseView = glm::inverse(view),
            .previousViewProjection = cameraData.previousViewProjection,
            .historyValid = !cameraCut,
            .cameraPosition = cameraPosition,
            .lightDirection = lightDirection,
            .lightRadiance = glm::vec3(cameraData.lightColor) * cameraData.lightDirection.w,
            .shadowCascades = shadowsEnabled ? cascadeCount : 0u,
            .lightViewProjections = cameraData.lightViewProjections,
            .cascadeSplits = cameraData.cascadeSplits,
            .ambientRadiance = (glm::vec3(cameraData.environmentSky) + glm::vec3(cameraData.environmentGround)) * 0.5f *
                               cameraData.environmentSky.w,
            .environmentIrradiance = imageBasedLighting,
            .ambientScale = cameraData.environmentSky.w,
            .irradianceVolumeMinimum = cameraData.irradianceVolumeMinimum,
            .irradianceVolumeCellSize = cameraData.irradianceVolumeCellSize,
            .irradianceVolumeCounts = cameraData.irradianceVolumeCounts,
            .medium = *fogMedium,
            .localLightCount = localLightCount,
            .shadowMap = m_shadowTexture.view,
            .shadowSampler = m_shadowSampler,
            .localLights = clusterBuffers.lights,
            .clusterRanges = clusterBuffers.clusterRanges,
            .clusterLightIndices = clusterBuffers.lightIndices,
            .environmentIrradianceBuffer = environmentState->primaryIrradiance,
            .irradianceVolumeProbes = m_irradianceVolume.buffer()};
        recordVolumetricFog(draw, &fogFrame);
    } else {
        recordVolumetricFog(draw, nullptr);
    }
    const auto opaqueCpuBegin = std::chrono::steady_clock::now();
    if (depthPrepass) {
        passTimestamps.write(FrameTimestamp::depthPrepassBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        if (!recordDepthPrepass(draw, gpuDrivenActive, depthImage, depthView, cameraData.viewProjection, lodView)) {
            return false;
        }
    }
    const auto prepassCpuEnd = std::chrono::steady_clock::now();
    m_depthPrepassFrame = depthPrepass ? std::optional<uint32_t>(frameIndex) : std::nullopt;
    recordAmbientOcclusion(draw, ambientOcclusion, projection,
                           rendering.settings().antialiasing == AntialiasingMode::Taa);
    m_postProcess.prepareScene(commandBuffer, frameIndex);
    const VkImageView sceneView = m_postProcess.sceneView(frameIndex);
    const VkImageView motionView = m_postProcess.motionView(frameIndex);
    beginRendering(commandBuffer, sceneView, motionView, depthImage, depthView, renderExtent,
                   depthPrepass ? SceneAttachments::ClearColorKeepDepth : SceneAttachments::Clear);
    m_depthInitialized = true;
    setSceneViewport(draw);
    const OpaquePipelines shadingOpaque{.singleSided = &shadingPipelines->opaque,
                                        .doubleSided = &shadingPipelines->doubleSided};
    if (gpuDrivenActive) {
        if (depthPrepass) {
            // The prepass culled both phases; shade the same compacted draws.
            recordGpuDrivenGroups(draw, GpuCullPhase::Early, shadingOpaque);
            recordGpuDrivenGroups(draw, GpuCullPhase::Late, shadingOpaque);
        } else {
            recordGpuDrivenGroups(draw, GpuCullPhase::Early, shadingOpaque);
            vkCmdEndRendering(commandBuffer);
            passTimestamps.write(FrameTimestamp::depthPyramidBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
            prepareDepthForPyramid(commandBuffer, depthImage);
            m_depthPyramid.build(commandBuffer, renderExtent);
            passTimestamps.write(FrameTimestamp::depthPyramidEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
            m_gpuDrivenPass.cull(commandBuffer, frameIndex, GpuCullPhase::Late, cameraData.viewProjection,
                                 m_depthPyramid, lodView);
            restoreDepthAfterPyramid(commandBuffer, depthImage);
            beginRendering(commandBuffer, sceneView, motionView, depthImage, depthView, renderExtent,
                           SceneAttachments::Preserve);
            setSceneViewport(draw);
            recordGpuDrivenGroups(draw, GpuCullPhase::Late, shadingOpaque);
        }
        stats.cpuOpaqueMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - opaqueCpuBegin).count();
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 2);
        }
        stats.visibleCount = m_gpuDrivenPass.retiredVisibleCount();
        if (shadowCulling == ShadowCulling::Gpu) {
            // Like visibleCount, a retired frame's count: casters drawn into
            // the cascades that frame redrew.
            stats.shadowCasterCount = m_gpuDrivenPass.retiredFrustumViewVisibleCount();
        }
        stats.indirectCommandCount = static_cast<uint32_t>(m_gpuDrivenBatches.size());
        stats.gpuVisibilityMismatches = m_gpuDrivenPass.visibilityMismatchCount();
    } else {
        stats.indirectCommandCount = static_cast<uint32_t>(m_visibility.commands.size());
    }
    const double prepassCpuMilliseconds =
        std::chrono::duration<double, std::milli>(prepassCpuEnd - opaqueCpuBegin).count();
    recordSelectionMask(draw);
    if (!gpuDrivenActive) {
        recordShadedItems(draw, ShadedItems::OpaqueOnly);
        // The GPU-driven timing above already spans the prepass.
        stats.cpuOpaqueMilliseconds += prepassCpuMilliseconds;
    }
    // The background fills what opaque geometry left at far depth, before
    // blended surfaces composite over it.
    if (imageBasedLighting && rendering.settings().environmentBackground) {
        passTimestamps.write(FrameTimestamp::skyboxBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        m_environmentPass.recordSkybox(commandBuffer, frameIndex, frameCamera.descriptor, environment.specularIntensity,
                                       cameraData.temporalJitter);
        passTimestamps.write(FrameTimestamp::skyboxEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    // Reflections of opaque surfaces and the background join the scene before
    // blended surfaces and outlines composite over it, and before the temporal
    // resolve accumulates it.
    if (!recordScreenSpaceReflections(
            draw, depthImage, depthView, rendering.settings(),
            {.projection = projection,
             .frameNumber = static_cast<uint32_t>(m_frameSerial),
             .temporalAccumulation = rendering.settings().antialiasing == AntialiasingMode::Taa})) {
        return false;
    }
    stats.reflectionBytes = m_postProcess.reflectionBytes();
    recordShadedItems(draw, ShadedItems::BlendedOnly);
    recordOutlines(draw);

    vkCmdEndRendering(commandBuffer);
    if (gpuDrivenActive) {
        m_gpuDrivenPass.finishFrame(commandBuffer, frameIndex);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 3);
    }
    passTimestamps.closeSkippedPasses();
    const auto bloomCpuBegin = std::chrono::steady_clock::now();
    const TemporalResolveFrame temporalResolve{.currentToPreviousClip = m_previousUnjitteredViewProjection *
                                                                        glm::inverse(unjitteredViewProjection),
                                               .jitterPixels = temporal.jitterPixels,
                                               .historyValid = temporal.historyValid};
    if (!m_postProcess.record(commandBuffer, frameIndex, colorImage, colorView, depthImage, extent, renderExtent,
                              rendering.settings(), glm::inverse(unjitteredViewProjection), m_previousViewProjection,
                              temporalResolve, cameraPosition, rendering.m_gridOverlay, timestampQueries,
                              stats.cpuBloomMilliseconds, colorOldLayout)) {
        return false;
    }
    m_previousViewProjection = cameraData.viewProjection;
    m_previousUnjitteredViewProjection = unjitteredViewProjection;
    m_previousJitterNdc = temporal.jitterNdc;
    for (RenderItem& item : m_renderItems) {
        item.previousWorld = item.world;
        item.previousJointPalette = item.jointPalette;
    }
    stats.cpuPostMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bloomCpuBegin).count() -
        stats.cpuBloomMilliseconds;
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 4);
    }
    return true;
}

void MeshPass::selectRenderItemLods(const MeshLodView& view) {
    for (RenderItem& item : m_renderItems) {
        const uint32_t level =
            selectMeshLod(*item.primitive, view, item.boundsCenter, item.boundsRadius, maximumAxisScale(item.world));
        item.lod = item.primitive->lod(level);
    }
}

bool MeshPass::prepareGpuDrivenFrame(uint32_t frameIndex, const MeshLodView& lodView) {
    m_gpuDrivenCandidates.clear();
    m_gpuDrivenKeys.clear();
    m_gpuDrivenKeySources.clear();
    // Without LOD selection the cull shader never leaves level 0, so skip the
    // coarser batches and their reserved instance slots.
    const bool lodEnabled = lodView.errorScale > 0.0f;
    for (const RenderItem& item : m_renderItems) {
        if (item.material->alphaMode == MaterialAlphaMode::Blend) {
            continue;
        }
        const GeometryAllocation& geometry = item.gpuMesh->geometry;
        const uint32_t candidate = static_cast<uint32_t>(m_gpuDrivenCandidates.size());
        m_gpuDrivenCandidates.push_back(&item);
        const uint32_t levelCount = lodEnabled ? item.primitive->lodCount() : 1u;
        for (uint32_t level = 0; level < levelCount; ++level) {
            const MeshLod lod = item.primitive->lod(level);
            m_gpuDrivenKeys.push_back({.pipeline = item.material->doubleSided ? doubleSidedBucket : singleSidedBucket,
                                       .geometryPage = geometry.page,
                                       .firstIndex = geometry.firstIndex + lod.firstIndex,
                                       .vertexOffset = geometry.vertexOffset,
                                       .indexCount = lod.indexCount});
            m_gpuDrivenKeySources.push_back({.candidate = candidate, .level = level});
        }
    }
    if (m_gpuDrivenKeys.size() > maximumGpuDrivenInstances) {
        std::cerr << "GPU-driven LOD instances exceed the renderer capacity\n";
        return false;
    }
    buildIndirectDrawPlan(m_gpuDrivenKeys, m_drawPlan);
    if (m_drawPlan.groups.size() > maximumGpuDrawGroups) {
        std::cerr << "GPU-driven draw groups exceed the renderer capacity\n";
        return false;
    }
    m_gpuDrivenBatches.clear();
    for (const IndirectDrawBatch& batch : m_drawPlan.batches) {
        m_gpuDrivenBatches.push_back({.indexCount = batch.key.indexCount,
                                      .firstIndex = batch.key.firstIndex,
                                      .vertexOffset = batch.key.vertexOffset,
                                      .firstInstance = batch.firstInstance,
                                      .group = batch.group,
                                      .groupFirstBatch = m_drawPlan.groups[batch.group].firstBatch});
    }
    m_gpuDrivenKeyBatches.resize(m_gpuDrivenKeys.size());
    for (size_t slot = 0; slot < m_drawPlan.instanceOrder.size(); ++slot) {
        m_gpuDrivenKeyBatches[m_drawPlan.instanceOrder[slot]] = m_drawPlan.instanceBatch[slot];
    }
    // Chain each primitive's levels so the cull shader can walk from level 0
    // to coarser batches. Every instance of a primitive writes the same links.
    for (size_t key = 0; key < m_gpuDrivenKeys.size(); ++key) {
        const GpuDrivenKeySource source = m_gpuDrivenKeySources[key];
        GpuDrivenBatch& batch = m_gpuDrivenBatches[m_gpuDrivenKeyBatches[key]];
        batch.lodGeometricError = m_gpuDrivenCandidates[source.candidate]->primitive->lod(source.level).geometricError;
        const bool hasCoarserLevel =
            key + 1 < m_gpuDrivenKeys.size() && m_gpuDrivenKeySources[key + 1].candidate == source.candidate;
        if (hasCoarserLevel) {
            batch.nextLodBatch = m_gpuDrivenKeyBatches[key + 1];
        }
    }
    m_gpuDrivenObjects.clear();
    for (size_t slot = 0; slot < m_drawPlan.instanceOrder.size(); ++slot) {
        const GpuDrivenKeySource source = m_gpuDrivenKeySources[m_drawPlan.instanceOrder[slot]];
        if (source.level != 0) {
            continue;
        }
        const RenderItem& item = *m_gpuDrivenCandidates[source.candidate];
        const uint32_t levelCount = lodEnabled ? item.primitive->lodCount() : 1u;
        // The persistent-scene slot is stable while the object lives, so it
        // carries the object's visibility from one frame to the next.
        m_gpuDrivenObjects.push_back(
            {.model = item.world,
             .previousModel = item.previousWorld,
             .bounds = {item.boundsCenter, item.boundsRadius},
             .draw = {m_drawPlan.instanceBatch[slot], item.gpuSceneHandle.index, levelCount, 0u},
             .skin = {item.skinPaletteOffset, static_cast<uint32_t>(item.jointPalette.size()), item.gpuMaterial->index,
                      0u}});
    }
    return m_gpuDrivenPass.prepare(frameIndex, m_gpuDrivenObjects, m_gpuDrivenBatches,
                                   static_cast<uint32_t>(m_visibility.visibleInstances.size()));
}

void MeshPass::bindOpaquePipeline(const DrawContext& draw, const GraphicsPipeline& pipeline, OpaquePipelines pipelines,
                                  VkDescriptorSet cameraSet) const {
    const VkDescriptorSet sets[]{cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0,
                            static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
    if (pipelines.pushesView) {
        vkCmdPushConstants(draw.commandBuffer, pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(draw.view),
                           &draw.view);
    }
}

void MeshPass::recordGpuDrivenGroups(const DrawContext& draw, GpuCullPhase phase, OpaquePipelines pipelines) {
    const GraphicsPipeline* boundPipeline = nullptr;
    for (uint32_t group = 0; group < m_drawPlan.groups.size(); ++group) {
        const IndirectDrawGroup& drawGroup = m_drawPlan.groups[group];
        const GraphicsPipeline& pipeline =
            drawGroup.pipeline == doubleSidedBucket ? *pipelines.doubleSided : *pipelines.singleSided;
        if (&pipeline != boundPipeline) {
            bindOpaquePipeline(draw, pipeline, pipelines, m_cameras[draw.frameIndex].gpuDrivenDescriptor);
            boundPipeline = &pipeline;
        }
        bindGeometryPage(draw.commandBuffer, drawGroup.geometryPage);
        m_gpuDrivenPass.drawGroup(draw.commandBuffer, GpuCullRegion::phase(phase), group, drawGroup.firstBatch,
                                  drawGroup.batchCount);
        ++draw.stats->drawCalls;
    }
}

bool MeshPass::recordDepthPrepass(const DrawContext& draw, bool gpuDrivenActive, VkImage depthImage,
                                  VkImageView depthView, const glm::mat4& viewProjection, const MeshLodView& lodView) {
    const VkCommandBuffer commandBuffer = draw.commandBuffer;
    const OpaquePipelines depthNormal{
        .singleSided = &m_depthNormalPipeline, .doubleSided = &m_depthNormalDoubleSidedPipeline, .pushesView = true};
    m_postProcess.prepareNormals(commandBuffer, draw.frameIndex);
    // Only screen-space reflections read specular reflectance, but the
    // depth-normal pipelines always declare the attachment: a null view with a
    // non-UNDEFINED pipeline format needs dynamicRenderingUnusedAttachments,
    // so the target is written whenever the prepass runs.
    if (!m_postProcess.prepareSpecular(commandBuffer, draw.frameIndex)) {
        return false;
    }
    const VkImageView normalView = m_postProcess.normalView(draw.frameIndex);
    const VkImageView specularView = m_postProcess.specularView(draw.frameIndex);
    beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                      SceneAttachments::Clear);
    setSceneViewport(draw);
    if (gpuDrivenActive) {
        // Two-phase occlusion culling runs here, so the shading pass reuses
        // both phases' compacted draws.
        recordGpuDrivenGroups(draw, GpuCullPhase::Early, depthNormal);
        vkCmdEndRendering(commandBuffer);
        prepareDepthForPyramid(commandBuffer, depthImage);
        m_depthPyramid.build(commandBuffer, draw.renderExtent);
        m_gpuDrivenPass.cull(commandBuffer, draw.frameIndex, GpuCullPhase::Late, viewProjection, m_depthPyramid,
                             lodView);
        restoreDepthAfterPyramid(commandBuffer, depthImage);
        beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                          SceneAttachments::Preserve);
        setSceneViewport(draw);
        recordGpuDrivenGroups(draw, GpuCullPhase::Late, depthNormal);
    } else {
        recordDepthNormalItems(draw);
    }
    vkCmdEndRendering(commandBuffer);
    m_postProcess.finishNormals(commandBuffer, draw.frameIndex);
    draw.passTimestamps->write(FrameTimestamp::depthPrepassEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    // Screen-space consumers get the pyramid of the complete opaque depth.
    draw.passTimestamps->write(FrameTimestamp::depthPyramidBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    prepareDepthForPyramid(commandBuffer, depthImage);
    m_depthPyramid.build(commandBuffer, draw.renderExtent);
    draw.passTimestamps->write(FrameTimestamp::depthPyramidEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    restoreDepthAfterPyramid(commandBuffer, depthImage);
    return true;
}

void MeshPass::recordDepthNormalItems(const DrawContext& draw) {
    const OpaquePipelines depthNormal{
        .singleSided = &m_depthNormalPipeline, .doubleSided = &m_depthNormalDoubleSidedPipeline, .pushesView = true};
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    // Opaque items precede blended ones (collectCpuSubmittedItems).
    for (size_t itemIndex = 0; itemIndex < m_visibleItems.size();) {
        const RenderItem& item = *m_visibleItems[itemIndex];
        if (item.material->alphaMode == MaterialAlphaMode::Blend) {
            break;
        }
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < m_visibleItems.size()) {
            const RenderItem& candidate = *m_visibleItems[batchEnd];
            // The shading pass must test EQUAL against the same LOD's depth.
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->alphaMode == MaterialAlphaMode::Blend ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        const GraphicsPipeline& pipeline =
            item.material->doubleSided ? m_depthNormalDoubleSidedPipeline : m_depthNormalPipeline;
        if (&pipeline != boundPipeline) {
            bindOpaquePipeline(draw, pipeline, depthNormal, m_cameras[draw.frameIndex].descriptor);
            boundPipeline = &pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, static_cast<uint32_t>(batchEnd - itemIndex),
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         mainObjectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
        itemIndex = batchEnd;
    }
}

bool MeshPass::recordScreenSpaceReflections(const DrawContext& draw, VkImage depthImage, VkImageView depthView,
                                            const RenderSettings& settings, const ScreenSpaceReflectionCamera& camera) {
    const SceneGeometryViews geometry = sceneGeometry(draw.frameIndex);
    const bool traces = PostProcessPass::tracesReflections(geometry, settings.reflections);
    if (traces) {
        vkCmdEndRendering(draw.commandBuffer);
    }
    if (!m_postProcess.recordReflections(draw.commandBuffer, draw.frameIndex, geometry, settings.reflections, camera,
                                         draw.timestampQueries)) {
        return false;
    }
    if (traces) {
        // The resumed pass loads the motion and depth attachments stored above;
        // recordReflections() ordered the HDR target itself.
        const VkMemoryBarrier2 attachments{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &attachments};
        vkCmdPipelineBarrier2(draw.commandBuffer, &dependency);
        beginRendering(draw.commandBuffer, m_postProcess.sceneView(draw.frameIndex),
                       m_postProcess.motionView(draw.frameIndex), depthImage, depthView, draw.renderExtent,
                       SceneAttachments::Preserve);
        setSceneViewport(draw);
    }
    return true;
}

SceneGeometryViews MeshPass::sceneGeometry(uint32_t frameIndex) const {
    if (m_depthPrepassFrame != frameIndex) {
        return {};
    }
    return {.normals = m_postProcess.normalView(frameIndex),
            .specular = m_postProcess.specularView(frameIndex),
            .depthPyramid = m_depthPyramid.view(),
            .depthPyramidSampler = m_depthPyramid.sampler(),
            .depthPyramidLevels = m_depthPyramid.levelCount(),
            .renderExtent = m_depthPyramid.renderExtent(),
            .storageExtent = m_depthPyramid.storageExtent()};
}

void MeshPass::recordVolumetricFog(const DrawContext& draw, const VolumetricFogFrame* frame) {
    if (frame == nullptr) {
        // Keeps the bound volume free of fog; records nothing once cleared.
        m_volumetricFog.recordCleared(draw.commandBuffer);
        return;
    }
    if (!m_shadowInitialized) {
        // Fog binds the shadow map even when no cascade was ever rendered; it
        // samples it only for rendered cascades.
        imageBarrier(draw.commandBuffer, m_shadowTexture.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, maximumShadowCascades);
        m_shadowInitialized = true;
    }
    draw.passTimestamps->write(FrameTimestamp::volumetricFogBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    m_volumetricFog.record(draw.commandBuffer, draw.frameIndex, *frame);
    draw.passTimestamps->write(FrameTimestamp::volumetricFogEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

void MeshPass::recordAmbientOcclusion(const DrawContext& draw, AmbientOcclusionQuality quality,
                                      const glm::mat4& projection, bool temporalNoise) {
    if (draw.timestampQueries) {
        vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, draw.timestampQueries, 10);
    }
    const SceneGeometryViews geometry = sceneGeometry(draw.frameIndex);
    if (quality == AmbientOcclusionQuality::Off || !geometry.valid()) {
        m_ambientOcclusion.recordUnoccluded(draw.commandBuffer, draw.frameIndex);
    } else {
        m_ambientOcclusion.record(
            draw.commandBuffer, draw.frameIndex, quality,
            {.normals = geometry.normals,
             .depthPyramid = geometry.depthPyramid,
             .depthPyramidSampler = geometry.depthPyramidSampler,
             .renderExtent = geometry.renderExtent,
             .inverseProjection = glm::inverse(projection),
             .projectionScale = 0.5f * static_cast<float>(geometry.renderExtent.height) * std::abs(projection[1][1]),
             .temporalNoise = temporalNoise});
    }
    if (draw.timestampQueries) {
        vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries, 11);
    }
}

void MeshPass::prepareDepthForPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const {
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The late pass loads the color targets the early pass stored.
    const VkMemoryBarrier2 colorBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &colorBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void MeshPass::restoreDepthAfterPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const {
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

bool MeshPass::collectCpuSubmittedItems(bool gpuDrivenActive, const glm::vec3& cameraPosition) {
    m_visibleItems.clear();
    for (GpuSceneHandle handle : m_visibility.visibleInstances) {
        const auto found = m_gpuHandleItems.find(handleKey(handle));
        if (found == m_gpuHandleItems.end()) {
            continue;
        }
        const RenderItem& item = m_renderItems[found->second];
        // With GPU culling the CPU only submits what the indirect path cannot:
        // blended surfaces and the editor's outline passes.
        if (gpuDrivenActive && item.material->alphaMode != MaterialAlphaMode::Blend && !item.outlined) {
            continue;
        }
        m_visibleItems.push_back(&item);
    }
    std::stable_sort(m_visibleItems.begin(), m_visibleItems.end(),
                     [&cameraPosition](const RenderItem* first, const RenderItem* second) {
                         const bool firstBlended = first->material->alphaMode == MaterialAlphaMode::Blend;
                         const bool secondBlended = second->material->alphaMode == MaterialAlphaMode::Blend;
                         if (firstBlended != secondBlended) {
                             return !firstBlended;
                         }
                         if (!firstBlended) {
                             // Opaque items arrive grouped by material, mesh and
                             // primitive; keeping each LOD's instances adjacent
                             // lets them share a draw.
                             return std::tuple(handleKey(first->materialHandle), handleKey(first->meshHandle),
                                               first->primitive->firstIndex, first->lod.firstIndex) <
                                    std::tuple(handleKey(second->materialHandle), handleKey(second->meshHandle),
                                               second->primitive->firstIndex, second->lod.firstIndex);
                         }
                         const glm::vec3 firstOffset = first->boundsCenter - cameraPosition;
                         const glm::vec3 secondOffset = second->boundsCenter - cameraPosition;
                         return glm::dot(firstOffset, firstOffset) > glm::dot(secondOffset, secondOffset);
                     });
    if (m_visibleItems.size() > maximumObjectsPerFrame) {
        std::cerr << "Visible object count exceeds the per-frame render capacity\n";
        return false;
    }
    return true;
}

void MeshPass::uploadCpuSubmittedObjects(uint32_t frameIndex) {
    auto* objectData = static_cast<ObjectData*>(m_cameras[frameIndex].objectsMapped) + mainObjectBase;
    for (size_t index = 0; index < m_visibleItems.size(); ++index) {
        const RenderItem& item = *m_visibleItems[index];
        objectData[index].model = item.world;
        objectData[index].previousModel = item.previousWorld;
        objectData[index].skin = {item.skinPaletteOffset, static_cast<uint32_t>(item.jointPalette.size()),
                                  item.gpuMaterial->index, 0};
    }
    vmaFlushAllocation(m_allocator, m_cameras[frameIndex].objects.allocation, sizeof(ObjectData) * mainObjectBase,
                       sizeof(ObjectData) * std::max<size_t>(m_visibleItems.size(), 1));
}

void MeshPass::recordSelectionMask(const DrawContext& draw) {
    if (std::ranges::none_of(m_visibleItems, [](const RenderItem* item) { return item->outlined; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.layout(), 0, 1,
                            &m_cameras[draw.frameIndex].descriptor, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < m_visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *m_visibleItems[itemIndex];
        if (!item.outlined) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         mainObjectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordShadedItems(const DrawContext& draw, ShadedItems selection) {
    const VkDescriptorSet sets[]{m_cameras[draw.frameIndex].descriptor, m_cameras[draw.frameIndex].bindlessDescriptor};
    const bool blendedOnly = selection == ShadedItems::BlendedOnly;
    const bool opaqueOnly = selection == ShadedItems::OpaqueOnly;
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    const auto opaqueCpuBegin = std::chrono::steady_clock::now();
    auto blendedCpuBegin = opaqueCpuBegin;
    // With GPU culling the opaque timestamp was already written after the
    // indirect draws.
    bool opaqueFinished = blendedOnly;
    for (size_t itemIndex = 0; itemIndex < m_visibleItems.size();) {
        const RenderItem& item = *m_visibleItems[itemIndex];
        const bool blended = item.material->alphaMode == MaterialAlphaMode::Blend;
        if ((blendedOnly && !blended) || (opaqueOnly && blended)) {
            ++itemIndex;
            continue;
        }
        if (!opaqueFinished && blended) {
            if (draw.timestampQueries) {
                vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries,
                                     2);
            }
            blendedCpuBegin = std::chrono::steady_clock::now();
            draw.stats->cpuOpaqueMilliseconds =
                std::chrono::duration<double, std::milli>(blendedCpuBegin - opaqueCpuBegin).count();
            opaqueFinished = true;
        }
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < m_visibleItems.size()) {
            const RenderItem& candidate = *m_visibleItems[batchEnd];
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->alphaMode != item.material->alphaMode ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        const GraphicsPipeline* pipeline = nullptr;
        if (blended) {
            pipeline = item.material->doubleSided ? &draw.pipelines->blendDoubleSided : &draw.pipelines->blend;
        } else {
            pipeline = item.material->doubleSided ? &draw.pipelines->doubleSided : &draw.pipelines->opaque;
        }
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
            vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0,
                                    static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
            boundPipeline = pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, instanceCount,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         mainObjectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
        draw.stats->visibleCount += instanceCount;
        draw.stats->triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
        itemIndex = batchEnd;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!opaqueFinished) {
        if (draw.timestampQueries) {
            vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries, 2);
        }
        draw.stats->cpuOpaqueMilliseconds = std::chrono::duration<double, std::milli>(now - opaqueCpuBegin).count();
    } else {
        draw.stats->cpuBlendedMilliseconds = std::chrono::duration<double, std::milli>(now - blendedCpuBegin).count();
    }
}

void MeshPass::recordOutlines(const DrawContext& draw) {
    if (std::ranges::none_of(m_visibleItems, [](const RenderItem* item) { return item->outlined; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.layout(), 0, 1,
                            &m_cameras[draw.frameIndex].descriptor, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < m_visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *m_visibleItems[itemIndex];
        if (!item.outlined) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        const glm::vec4 outlineSettings{item.mesh->bounds.center, 2.0f};
        vkCmdPushConstants(draw.commandBuffer, m_outlinePipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(outlineSettings), &outlineSettings);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         mainObjectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordDeferredUploads(const RenderSettings& settings, RenderStats& stats) {
    m_deferredUploads.setConfiguration({.warningFrames = settings.deferredUploadWarningFrames});
    const DeferredUploadFrameSummary deferred = m_deferredUploads.endFrame();
    stats.deferredMeshUploads = deferred.deferredMeshes;
    stats.deferredTextureUploads = deferred.deferredTextures;
    for (const DeferredUploadWarning& warning : deferred.overdue) {
        std::cerr << describeDeferredUpload(warning) << '\n';
    }
}

void MeshPass::pruneUnusedAssets(const AssetManager& assets) {
    // Asset leases keep an asset alive until every frame that drew it retired,
    // so geometry ranges and material indices can be reused immediately.
    std::erase_if(m_meshes, [&](const auto& entry) {
        if (assets.mesh(handleFromKey<MeshHandle>(entry.first)) != nullptr) {
            return false;
        }
        m_geometry->free(entry.second.geometry);
        return true;
    });
    std::erase_if(m_unpackableMeshes,
                  [&](uint64_t key) { return assets.mesh(handleFromKey<MeshHandle>(key)) == nullptr; });
    std::erase_if(m_rejectedTextures,
                  [&](uint64_t key) { return assets.texture(handleFromKey<TextureHandle>(key)) == nullptr; });
    std::erase_if(m_textures, [&](const auto& entry) {
        if (assets.texture(handleFromKey<TextureHandle>(entry.first)) != nullptr) {
            return false;
        }
        retireTexture(entry.second);
        return true;
    });
    std::erase_if(m_materials, [&](const auto& entry) {
        if (assets.material(handleFromKey<MaterialHandle>(entry.first)) != nullptr) {
            return false;
        }
        m_freeMaterialIndices.push_back(entry.second.index);
        return true;
    });
}

bool MeshPass::uiTexture(TextureHandle handle, const AssetManager& assets, VkSampler& sampler, VkImageView& view) {
    const GpuTexture* texture = ensureTexture(handle, assets);
    if (!texture) {
        return false;
    }
    sampler = m_sampler;
    view = texture->srgbView ? texture->srgbView : texture->view;
    return true;
}

void MeshPass::shutdown(const VulkanContext& context) {
    if (m_gpuDrivenSupported) {
        m_gpuDrivenPass.shutdown(context);
    }
    m_gpuDrivenSupported = false;
    m_clusteredLighting.shutdown();
    m_reflectionProbeClusters.shutdown();
    m_uploadContext.shutdown();
    m_textureUploadContext.shutdown();
    for (auto& [variant, pipelines] : m_meshPipelines) {
        pipelines->opaque.shutdown(context.device());
        pipelines->doubleSided.shutdown(context.device());
        pipelines->blend.shutdown(context.device());
        pipelines->blendDoubleSided.shutdown(context.device());
    }
    m_meshPipelines.clear();
    m_shadowPipeline.shutdown(context.device());
    m_selectionMaskPipeline.shutdown(context.device());
    m_outlinePipeline.shutdown(context.device());
    m_environmentPass.shutdown();
    m_irradianceVolume.shutdown();
    m_depthNormalPipeline.shutdown(context.device());
    m_depthNormalDoubleSidedPipeline.shutdown(context.device());
    m_postProcess.shutdown(context);
    m_depthPyramid.shutdown();
    m_ambientOcclusion.shutdown();
    m_volumetricFog.shutdown();
    m_fogDescriptorGenerations.clear();
    m_depthPrepassFrame.reset();
    m_meshes.clear();
    m_unpackableMeshes.clear();
    m_rejectedTextures.clear();
    m_deferredUploads = DeferredUploadTracker{};
    m_meshUploadsPending = false;
    for (const GeometryPage& page : m_geometryPages) {
        destroyGeometryPage(page);
    }
    m_geometryPages.clear();
    if (m_zeroSkinInfluence.buffer) {
        vmaDestroyBuffer(context.vmaAllocator(), m_zeroSkinInfluence.buffer, m_zeroSkinInfluence.allocation);
    }
    m_zeroSkinInfluence = {};
    m_geometry.reset();
    // Shutdown runs after the device is idle, so deferred releases may run now.
    m_releaseQueue.collect(std::numeric_limits<uint64_t>::max());
    // After the queue ran: it may hold atlases this pass retired.
    m_localShadows.shutdown();
    m_changedShadowCasters.clear();
    for (auto& [_, texture] : m_textures) {
        destroyTexture(texture);
    }
    m_textures.clear();
    m_textureTable.reset();
    m_freeMaterialIndices.clear();
    m_nextMaterialIndex = 0;
    const auto destroyOwnedTexture = [&](GpuTexture& texture) {
        if (texture.srgbView) {
            vkDestroyImageView(context.device(), texture.srgbView, nullptr);
        }
        if (texture.view) {
            vkDestroyImageView(context.device(), texture.view, nullptr);
        }
        if (texture.image) {
            vmaDestroyImage(context.vmaAllocator(), texture.image, texture.allocation);
        }
        texture = {};
    };
    destroyOwnedTexture(m_whiteTexture);
    destroyOwnedTexture(m_normalTexture);
    for (VkImageView& view : m_shadowLayerViews) {
        if (view) {
            vkDestroyImageView(context.device(), view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    destroyOwnedTexture(m_shadowTexture);
    m_materials.clear();
    for (auto& camera : m_cameras) {
        if (camera.buffer.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.buffer.buffer, camera.buffer.allocation);
        }
        if (camera.objects.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.objects.buffer, camera.objects.allocation);
        }
        if (camera.skinPalettes.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.skinPalettes.buffer, camera.skinPalettes.allocation);
        }
        if (camera.previousSkinPalettes.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.previousSkinPalettes.buffer,
                             camera.previousSkinPalettes.allocation);
        }
        if (camera.materials.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.materials.buffer, camera.materials.allocation);
        }
    }

    m_cameras.clear();

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(context.device(), m_descriptorPool, nullptr);
    }
    if (m_bindlessPool) {
        vkDestroyDescriptorPool(context.device(), m_bindlessPool, nullptr);
    }

    if (m_cameraLayout) {
        vkDestroyDescriptorSetLayout(context.device(), m_cameraLayout, nullptr);
    }
    if (m_bindlessLayout) {
        vkDestroyDescriptorSetLayout(context.device(), m_bindlessLayout, nullptr);
    }
    if (m_sampler) {
        vkDestroySampler(context.device(), m_sampler, nullptr);
    }
    if (m_shadowSampler) {
        vkDestroySampler(context.device(), m_shadowSampler, nullptr);
    }

    m_descriptorPool = VK_NULL_HANDLE;
    m_bindlessPool = VK_NULL_HANDLE;
    m_cameraLayout = VK_NULL_HANDLE;
    m_bindlessLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_shadowSampler = VK_NULL_HANDLE;
    m_shadowInitialized = false;
    m_depthInitialized = false;
}
