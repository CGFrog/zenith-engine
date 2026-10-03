#include "MeshPass.hpp"
#include "../Decals.hpp"
#include "../DepthConvention.hpp"
#include "../IrradianceVolume.hpp"
#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"
#include "../RenderViewPlanning.hpp"
#include "../SpatialUpscale.hpp"
#include "../ViewShading.hpp"
#include "../resources/DescriptorPoolSizes.hpp"
#include "../resources/MipChainBlit.hpp"

#include "engine/assets/CompressedTexture.hpp"
#include "engine/assets/PackedVertexStreams.hpp"
#include "engine/assets/TextureMipStreaming.hpp"
#include "engine/graphics/MaterialRecord.hpp"
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

// Replaces the scene's directional and hemisphere light with the studio rig
// for view shading modes other than Rendered (see ViewShading.hpp).
void applyStudioLighting(CameraData& cameraData, const StudioLighting& studio) {
    cameraData.lightDirection = {studio.keyDirection, 1.0f};
    cameraData.lightColor = {studio.keyRadiance, 1.0f};
    cameraData.environmentSky = {studio.skyColor, studio.diffuseIntensity};
    cameraData.environmentGround = {studio.groundColor, studio.specularIntensity};
    for (size_t level = 0; level < studio.specularLevels.size(); ++level) {
        cameraData.environmentSpecular[level] = {studio.specularLevels[level], 1.0f};
    }
}

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
                                           const glm::vec3& cameraPosition, bool perPixelProbes,
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
    if (perPixelProbes) {
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

// Bindings of the camera set (set 0 of the mesh, shadow, outline, particle,
// and skybox pipelines). Every view's camera set uses this layout.
std::vector<VkDescriptorSetLayoutBinding> cameraLayoutBindings() {
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
} // namespace

bool MeshPass::init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
                    VkExtent2D extent, VkImageView depthView) {
    m_log = context.log();
    m_device = context.device();
    m_pipelineCache = context.pipelineCache();
    m_allocator = context.vmaAllocator();
    m_blockCompressionSupported = context.blockCompressionSupported();
    m_packetCachingEnabled = SDL_getenv("ZENITH_DISABLE_RENDER_PACKET_CACHE") == nullptr;
    m_outputFormat = colorFormat;
    m_renderItems.reserve(maximumObjectsPerFrame);
    m_shadowVisibleItems.reserve(maximumObjectsPerFrame);
    m_objectItems.reserve(maximumObjectsPerFrame);
    m_gpuRecords.reserve(maximumObjectsPerFrame);
    m_gpuHandleItems.reserve(maximumObjectsPerFrame);
    m_gpuDrivenObjects.reserve(maximumObjectsPerFrame);
    m_gpuDrivenBatches.reserve(maximumObjectsPerFrame);
    m_gpuDrivenCandidates.reserve(maximumObjectsPerFrame);
    m_gpuDrivenKeys.reserve(maximumObjectsPerFrame);
    if (!m_uploadContext.init(context, true) || !m_textureUploadContext.init(context, false) ||
        !createDescriptors(context, frameCount)) {
        return false;
    }
    TextureAsset white{.width = 1, .height = 1, .rgba = {255, 255, 255, 255}};
    TextureAsset normal{.width = 1, .height = 1, .rgba = {128, 128, 255, 255}};
    m_geometry = std::make_unique<GeometryArena>(defaultGeometryPage);
    if (!createZeroSkinInfluence() || !uploadTexture(white, m_whiteTexture) ||
        !uploadTexture(normal, m_normalTexture) || !createBindlessDescriptors(context, frameCount) ||
        !createShadowResources(depthFormat) ||
        !m_localShadows.init(context, frameCount, depthFormat, m_cameraLayout, m_bindlessLayout) ||
        !createPipeline(PostProcessPass::sceneFormat, depthFormat) ||
        !m_environmentPass.init(context, m_cameraLayout, PostProcessPass::sceneFormat, PostProcessPass::motionFormat,
                                depthFormat, frameCount) ||
        !m_particles.init(context, frameCount, m_cameraLayout, PostProcessPass::sceneFormat,
                          PostProcessPass::motionFormat, depthFormat) ||
        !m_idBuffer.init(context, m_pipelineCache, m_cameraLayout, depthFormat)) {
        return false;
    }
    // The default view draws through the frame's camera sets; its bindings are
    // written before the GPU-driven sets copy the shared ones.
    m_views.clear();
    m_views.push_back(std::make_unique<ViewState>());
    ViewState& defaultView = *m_views.front();
    defaultView.sharesFrameSets = true;
    if (!initViewPasses(context, defaultView, extent, depthView)) {
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
    m_gpuDrivenSupported = m_gpuDrivenPass.init(context, frameCount, defaultView.depthPyramid, limits,
                                                features12.drawIndirectCount == VK_TRUE);
    if (m_gpuDrivenSupported && !createGpuDrivenDescriptors()) {
        return false;
    }
    writeAmbientOcclusionDescriptors(defaultView);
    writeTransmissionBackgroundDescriptors(defaultView);
    if (!m_irradianceVolume.init(context, frameCount)) {
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const std::array<VkDescriptorSet, 2> sets{m_cameras[frame].descriptor, m_cameras[frame].gpuDrivenDescriptor};
        m_irradianceVolume.writeDescriptors(frame, sets);
    }
    return true;
}

bool MeshPass::initViewPasses(const VulkanContext& context, ViewState& view, VkExtent2D extent, VkImageView depthView) {
    const uint32_t frameCount = static_cast<uint32_t>(m_cameras.size());
    view.visibleItems.reserve(maximumObjectsPerFrame);
    view.visibleOutlined.reserve(maximumObjectsPerFrame);
    view.visibility.visibleInstances.reserve(maximumObjectsPerFrame);
    view.visibility.commands.reserve(maximumObjectsPerFrame);
    if (!view.clusteredLighting.init(context, frameCount, ClusterGridConfiguration{}, maximumLocalLights) ||
        !view.reflectionProbeClusters.init(context, frameCount, view.clusteredLighting.configuration()) ||
        !view.decalClusters.init(context, frameCount, view.clusteredLighting.configuration()) ||
        !view.volumetricFog.init(context, frameCount) ||
        !view.postProcess.init(context, m_outputFormat, m_sceneDepthFormat, frameCount, extent, depthView) ||
        !view.depthPyramid.init(context, extent, depthView) ||
        !view.ambientOcclusion.init(context, frameCount, extent) ||
        !view.transmissionBackground.init(context, extent)) {
        return false;
    }
    view.postProcess.setFixedFrameSeconds(m_fixedFrameSeconds);
    writeClusteredLightingDescriptors(view);
    writeReflectionProbeDescriptors(view);
    writeDecalDescriptors(view);
    view.fogDescriptorGenerations.assign(frameCount, 0);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        writeVolumetricFogDescriptors(view, frame);
    }
    writeAmbientOcclusionDescriptors(view);
    writeTransmissionBackgroundDescriptors(view);
    return true;
}

bool MeshPass::createView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView) {
    if (view == 0 || hasView(view)) {
        m_log.error("Render view slot " + std::to_string(view) + " cannot be created");
        return false;
    }
    if (view >= m_views.size()) {
        m_views.resize(view + 1);
    }
    auto state = std::make_unique<ViewState>();
    if (!createViewCameraSets(*state) || !initViewPasses(context, *state, extent, depthView)) {
        destroyViewState(context, *state);
        return false;
    }
    m_views[view] = std::move(state);
    return true;
}

bool MeshPass::resizeView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView) {
    if (!hasView(view)) {
        return false;
    }
    ViewState& state = *m_views[view];
    state.depthInitialized = false;
    state.cameraHistoryValid = false;
    state.temporalQuality.invalidateHistory();
    state.depthPrepassFrame.reset();
    if (!state.postProcess.resize(context, extent, depthView) || !state.depthPyramid.resize(extent, depthView) ||
        !state.ambientOcclusion.resize(extent) || !state.transmissionBackground.resize(extent)) {
        return false;
    }
    writeAmbientOcclusionDescriptors(state);
    writeTransmissionBackgroundDescriptors(state);
    if (view == 0 && m_gpuDrivenSupported) {
        m_gpuDrivenPass.bindDepthPyramid(state.depthPyramid);
    }
    return true;
}

void MeshPass::destroyView(const VulkanContext& context, uint32_t view) {
    if (view == 0 || !hasView(view)) {
        return;
    }
    destroyViewState(context, *m_views[view]);
    m_views[view].reset();
}

bool MeshPass::createViewCameraSets(ViewState& view) {
    const uint32_t frameCount = static_cast<uint32_t>(m_cameras.size());
    const std::vector<VkDescriptorSetLayoutBinding> bindings = cameraLayoutBindings();
    const std::vector<VkDescriptorPoolSize> poolSizes = descriptorPoolSizes(bindings, frameCount);
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                              .pPoolSizes = poolSizes.data()};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &view.descriptorPool),
                     "Create render view descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = view.descriptorPool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate render view camera descriptor sets")) {
        return false;
    }
    view.cameras.resize(frameCount);
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        ViewCameraSet& camera = view.cameras[frame];
        camera.descriptor = sets[frame];
        VmaAllocationInfo info{};
        VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = sizeof(CameraData),
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &camera.uniform.buffer,
                                         &camera.uniform.allocation, &info),
                         "Create render view camera buffer")) {
            return false;
        }
        camera.uniformMapped = info.pMappedData;
        buffer.size = sizeof(ObjectData) * maximumObjectsPerFrame;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &camera.objects.buffer,
                                         &camera.objects.allocation, &info),
                         "Create render view object buffer")) {
            return false;
        }
        camera.objectsMapped = info.pMappedData;
        const VkDescriptorBufferInfo uniformInfo{.buffer = camera.uniform.buffer, .range = sizeof(CameraData)};
        const VkDescriptorBufferInfo objectInfo{.buffer = camera.objects.buffer, .range = VK_WHOLE_SIZE};
        const std::array<VkWriteDescriptorSet, 2> writes{{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = camera.descriptor,
                                                           .dstBinding = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pBufferInfo = &uniformInfo},
                                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = camera.descriptor,
                                                           .dstBinding = 2,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pBufferInfo = &objectInfo}}};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}

void MeshPass::destroyViewState(const VulkanContext& context, ViewState& view) {
    view.clusteredLighting.shutdown();
    view.reflectionProbeClusters.shutdown();
    view.decalClusters.shutdown();
    // The caller idled the device, so every retired fog volume may go.
    view.volumetricFog.collect(std::numeric_limits<uint64_t>::max());
    view.volumetricFog.shutdown();
    view.fogDescriptorGenerations.clear();
    view.postProcess.shutdown(context);
    view.depthPyramid.shutdown();
    view.ambientOcclusion.shutdown();
    view.transmissionBackground.shutdown();
    for (ViewCameraSet& camera : view.cameras) {
        if (camera.uniform.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.uniform.buffer, camera.uniform.allocation);
        }
        if (camera.objects.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.objects.buffer, camera.objects.allocation);
        }
    }
    view.cameras.clear();
    if (view.descriptorPool) {
        vkDestroyDescriptorPool(context.device(), view.descriptorPool, nullptr);
    }
    view.descriptorPool = VK_NULL_HANDLE;
    view.depthPrepassFrame.reset();
    view.depthInitialized = false;
}

VkDescriptorSet MeshPass::viewCameraSet(const ViewState& view, uint32_t frameIndex) const {
    return view.sharesFrameSets ? m_cameras[frameIndex].descriptor : view.cameras[frameIndex].descriptor;
}

std::array<VkDescriptorSet, 2> MeshPass::viewCameraSets(const ViewState& view, uint32_t frameIndex) const {
    if (view.sharesFrameSets) {
        return {m_cameras[frameIndex].descriptor, m_cameras[frameIndex].gpuDrivenDescriptor};
    }
    return {view.cameras[frameIndex].descriptor, VK_NULL_HANDLE};
}

void MeshPass::copySharedCameraBindings(const ViewState& view, uint32_t frameIndex) {
    if (view.sharesFrameSets) {
        return;
    }

    struct SharedBinding {
        uint32_t binding;
        uint32_t count;
    };

    const SharedBinding shared[]{{1, 1},
                                 {3, 1},
                                 {4, 1},
                                 {EnvironmentPass::specularBinding, EnvironmentPass::environmentSlots},
                                 {EnvironmentPass::irradianceBinding, EnvironmentPass::environmentSlots},
                                 {EnvironmentPass::brdfBinding, 1},
                                 {IrradianceVolumePass::binding, 1},
                                 {previousSkinPaletteBinding, 1},
                                 {LocalShadowPass::atlasBinding, 1},
                                 {LocalShadowPass::tileBinding, 1},
                                 {EnvironmentPass::probeSpecularBinding, 1}};
    std::array<VkCopyDescriptorSet, std::size(shared)> copies{};
    for (size_t index = 0; index < copies.size(); ++index) {
        copies[index] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                         .srcSet = m_cameras[frameIndex].descriptor,
                         .srcBinding = shared[index].binding,
                         .dstSet = view.cameras[frameIndex].descriptor,
                         .dstBinding = shared[index].binding,
                         .descriptorCount = shared[index].count};
    }
    vkUpdateDescriptorSets(m_device, 0, nullptr, static_cast<uint32_t>(copies.size()), copies.data());
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
        std::array<VkCopyDescriptorSet, 14> copies{};
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
                                        volumetricFogBinding,
                                        DecalClusterPass::recordsBinding,
                                        DecalClusterPass::clusterListsBinding};
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

void MeshPass::writeClusteredLightingDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const ClusteredLightingPass::FrameBuffers buffers = view.clusteredLighting.frameBuffers(frame);
        // Bindings 9-11: local lights, per-cluster ranges, cluster light indices.
        const std::array<VkDescriptorBufferInfo, 3> infos{{{.buffer = buffers.lights, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.clusterRanges, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.lightIndices, .range = VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = viewCameraSet(view, frame),
                             .dstBinding = 9 + index,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[index]};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void MeshPass::writeReflectionProbeDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorBufferInfo probes{.buffer = view.reflectionProbeClusters.frameBuffer(frame),
                                            .range = VK_WHOLE_SIZE};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = viewCameraSet(view, frame),
                                         .dstBinding = ReflectionProbeClusterPass::cameraBinding,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &probes};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void MeshPass::writeDecalDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const DecalClusterPass::FrameBuffers buffers = view.decalClusters.frameBuffers(frame);
        const std::array<VkDescriptorBufferInfo, 2> infos{{{.buffer = buffers.records, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.clusterLists, .range = VK_WHOLE_SIZE}}};
        const std::array<uint32_t, 2> bindings{DecalClusterPass::recordsBinding, DecalClusterPass::clusterListsBinding};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = viewCameraSet(view, frame),
                             .dstBinding = bindings[index],
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[index]};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

std::vector<GpuDecal> MeshPass::prepareDecals(const Scene& scene, const AssetManager& assets,
                                              const RenderSettings& settings, const glm::vec3& cameraPosition,
                                              const glm::mat4& view, uint32_t& sceneDecalCount) {
    sceneDecalCount = 0;
    if (settings.decals == DecalQuality::Off) {
        return {};
    }
    const std::vector<DecalProjector> sceneDecals = gatherDecals(scene);
    sceneDecalCount = static_cast<uint32_t>(sceneDecals.size());
    // Textures resolve before the frame's bindless writes. A decal whose
    // texture is still uploading (or was rejected) is left out until it lands,
    // rather than drawing untextured.
    const auto slotFor = [&](TextureHandle handle, bool srgb) -> std::optional<uint32_t> {
        const GpuTexture* texture = handle ? ensureTexture(handle, assets) : nullptr;
        if (!texture) {
            return std::nullopt;
        }
        return srgb ? texture->srgbSlot : texture->linearSlot;
    };
    std::vector<GpuDecal> records;
    for (const DecalProjector& decal : selectDecals(sceneDecals, cameraPosition, maximumDecals)) {
        const DecalTextureSlots slots{.albedo = slotFor(decal.albedoTexture, true),
                                      .normal = slotFor(decal.normalTexture, false)};
        if ((decal.albedoTexture && !slots.albedo) || (decal.normalTexture && !slots.normal)) {
            continue;
        }
        records.push_back(packDecal(decal, slots, view));
    }
    return records;
}

void MeshPass::writeAmbientOcclusionDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorImageInfo occlusion{.sampler = view.ambientOcclusion.sampler(),
                                              .imageView = view.ambientOcclusion.occlusionView(frame),
                                              .imageLayout = AmbientOcclusionPass::occlusionLayout};
        // The GPU-driven set is written directly: copying would need the
        // CPU-submission set written first on every resize.
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t writeCount = 0;
        for (VkDescriptorSet set : viewCameraSets(view, frame)) {
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

void MeshPass::writeTransmissionBackgroundDescriptors(const ViewState& view) {
    const VkDescriptorImageInfo background{.sampler = view.transmissionBackground.sampler(),
                                           .imageView = view.transmissionBackground.view(),
                                           .imageLayout = TransmissionBackground::readLayout};
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t writeCount = 0;
        for (VkDescriptorSet set : viewCameraSets(view, frame)) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = set,
                                    .dstBinding = transmissionBackgroundBinding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .pImageInfo = &background};
        }
        vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    }
}

void MeshPass::writeVolumetricFogDescriptors(ViewState& view, uint32_t frameIndex) {
    if (view.fogDescriptorGenerations[frameIndex] == view.volumetricFog.generation()) {
        return;
    }
    const VkDescriptorImageInfo fog{.sampler = view.volumetricFog.sampler(),
                                    .imageView = view.volumetricFog.integratedView(),
                                    .imageLayout = VolumetricFogPass::volumeLayout};
    // The frame slot's previous submission retired, so its sets are not in use.
    std::array<VkWriteDescriptorSet, 2> writes{};
    uint32_t writeCount = 0;
    for (VkDescriptorSet set : viewCameraSets(view, frameIndex)) {
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
    view.fogDescriptorGenerations[frameIndex] = view.volumetricFog.generation();
}

bool MeshPass::createDescriptors(const VulkanContext& context, uint32_t frameCount) {
    const std::vector<VkDescriptorSetLayoutBinding> cameraBindings = cameraLayoutBindings();
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(std::size(cameraBindings)),
        .pBindings = cameraBindings.data(),
    };

    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_cameraLayout),
                     "Create camera descriptor layout")) {
        return false;
    }

    // Each frame has a CPU-submission camera set and a GPU-driven one that reads
    // instances from the culling output instead of the host object buffer.
    const uint32_t cameraSetCount = frameCount * 2;
    const std::vector<VkDescriptorPoolSize> poolSizes = descriptorPoolSizes(cameraBindings, cameraSetCount);
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = cameraSetCount,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
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
    // frame capture. ZENITH_FORCE_UPDATE_AFTER_BIND takes that path on any
    // device that supports it, so it can be validated where it is not needed.
    const auto& limits = properties.properties.limits;
    const uint32_t ordinaryLimit =
        std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
    const uint32_t updateAfterBindLimit = std::min(properties12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                   properties12.maxDescriptorSetUpdateAfterBindSampledImages);
    const bool forceUpdateAfterBind = SDL_getenv("ZENITH_FORCE_UPDATE_AFTER_BIND") != nullptr;
    const bool updateAfterBind =
        features12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        (forceUpdateAfterBind ||
         (ordinaryLimit < maximumBindlessTextures + reservedSampledImages && updateAfterBindLimit > ordinaryLimit));
    if (forceUpdateAfterBind) {
        context.log().info(updateAfterBind ? "Bindless materials use update-after-bind (ZENITH_FORCE_UPDATE_AFTER_BIND)"
                                           : "ZENITH_FORCE_UPDATE_AFTER_BIND ignored: the device lacks sampled-image "
                                             "update-after-bind");
    }
    const uint32_t deviceLimit = updateAfterBind ? updateAfterBindLimit : ordinaryLimit;
    if (deviceLimit <= reservedSampledImages + 3) {
        context.log().error("The device exposes too few sampled-image descriptors for bindless materials");
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
        VkBool32 solidSurfaces;
    };

    const Constants constants{.physicallyBased = variant.physicallyBased ? VK_TRUE : VK_FALSE,
                              .shadowCascades = static_cast<int32_t>(variant.shadowCascades),
                              .solidSurfaces = variant.solidSurfaces ? VK_TRUE : VK_FALSE};
    const VkSpecializationMapEntry entries[]{
        {.constantID = 0, .offset = offsetof(Constants, physicallyBased), .size = sizeof(VkBool32)},
        {.constantID = 1, .offset = offsetof(Constants, shadowCascades), .size = sizeof(int32_t)},
        {.constantID = 2, .offset = offsetof(Constants, solidSurfaces), .size = sizeof(VkBool32)}};
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
    bool created = create(pipelines->opaque, false, false) && create(pipelines->doubleSided, true, false) &&
                   create(pipelines->blend, false, true) && create(pipelines->blendDoubleSided, true, true);
    if (created && variant.wireframeOverlay) {
        created = createWireframeOverlay(*pipelines, vertex.handle());
    }
    if (!created) {
        pipelines->shutdown(m_device);
        return nullptr;
    }
    return m_meshPipelines.emplace(variant, std::move(pipelines)).first->second.get();
}

bool MeshPass::createWireframeOverlay(MeshPipelineSet& pipelines, VkShaderModule meshVertex) {
    ShaderModule fragment(m_device);
    if (!fragment.load(shaderRoot() / "mesh_wireframe.frag.spv")) {
        return false;
    }
    const auto create = [&](GraphicsPipeline& pipeline, bool doubleSided) {
        return pipeline.initWireframeOverlayPipeline(m_device, m_sceneColorFormat, PostProcessPass::motionFormat,
                                                     m_sceneDepthFormat, m_cameraLayout, m_bindlessLayout, meshVertex,
                                                     fragment.handle(), m_pipelineCache, doubleSided);
    };
    return create(pipelines.wireframe, false) && create(pipelines.wireframeDoubleSided, true);
}

void MeshPass::MeshPipelineSet::shutdown(VkDevice device) {
    opaque.shutdown(device);
    doubleSided.shutdown(device);
    blend.shutdown(device);
    blendDoubleSided.shutdown(device);
    wireframe.shutdown(device);
    wireframeDoubleSided.shutdown(device);
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

namespace {
struct BlockCompressedFormats {
    VkFormat linear{VK_FORMAT_UNDEFINED};
    // VK_FORMAT_UNDEFINED when the format has no sRGB variant (BC5).
    VkFormat srgb{VK_FORMAT_UNDEFINED};
};

// ensureTexture already rejected every other block-compressed encoding.
BlockCompressedFormats blockCompressedFormats(const TextureAsset& texture) {
    if (texture.format == TextureFormat::Bc7) {
        return {.linear = VK_FORMAT_BC7_UNORM_BLOCK, .srgb = VK_FORMAT_BC7_SRGB_BLOCK};
    }
    return {.linear = VK_FORMAT_BC5_UNORM_BLOCK};
}

// Streamed textures start with the levels that fit in this many texels per
// side: a few kilobytes that make the texture sampleable at once.
constexpr uint32_t streamedTextureTailExtent = 64;
// Staged mip levels start on a texel-block boundary (16 bytes for BC5/BC7).
constexpr VkDeviceSize mipStagingAlignment = 16;
// Largest batch of streamed levels staged in one frame. Every streamed level
// fits (textureRejectionReason bounds a whole chain by the slot size), and the
// slack absorbs per-level alignment padding.
constexpr uint64_t maximumMipStreamingBatchBytes = UploadContext::maximumSlotBytes / 2;
} // namespace

bool MeshPass::uploadTexture(const TextureAsset& texture, GpuTexture& output) {
    if (texture.blockCompressed()) {
        // Streamed textures start with their mip tail; Off uploads every level now.
        const uint32_t firstMip =
            !textureStreamingActive()
                ? 0u
                : textureMipTailFirstLevel(texture.width, texture.height, static_cast<uint32_t>(texture.mips.size()),
                                           streamedTextureTailExtent);
        return uploadBlockCompressedTexture(texture, output, firstMip);
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

    const uint32_t mipLevels = fullMipChainLength(texture.width, texture.height);
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
    output.mipCount = mipLevels;

    // A dedicated transfer queue's consumers wait on the upload timeline
    // semaphore, so its final barriers only order the transfer work.
    const MipChainConsumers consumers =
        m_textureUploadContext.usesDedicatedQueue()
            ? MipChainConsumers{.stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .access = 0}
            : MipChainConsumers{.stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                            .srcAccessMask = 0,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = output.image,
                                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                 .baseMipLevel = 0,
                                                                 .levelCount = mipLevels,
                                                                 .baseArrayLayer = 0,
                                                                 .layerCount = 1}};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(command, &dependency);
        VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .mipLevel = 0,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1},
                               .imageExtent = {.width = texture.width, .height = texture.height, .depth = 1}};
        vkCmdCopyBufferToImage(command, staging, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        recordMipChainBlits(
            command, {.image = output.image, .width = texture.width, .height = texture.height, .mipCount = mipLevels},
            consumers);
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

// Cooked textures carry their full mip chain. The image always holds every
// level, but only [firstMip, mipCount) is uploaded here; streamTextureMips
// adds the rest. BC7 gets UNORM and sRGB views; BC5 has no sRGB variant and
// gets one view.
bool MeshPass::uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output, uint32_t firstMip) {
    const auto uploadBegin = std::chrono::steady_clock::now();
    const BlockCompressedFormats formats = blockCompressedFormats(texture);
    const uint32_t mipLevels = static_cast<uint32_t>(texture.mips.size());
    firstMip = std::min(firstMip, mipLevels - 1);
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = formats.srgb ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0u,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = formats.linear,
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
    output.mipCount = mipLevels;
    output.residentFirstMip = mipLevels;
    // Nothing references the image until this returns, so failures release it directly.
    const auto fail = [&] {
        destroyTexture(output);
        output = {};
        return false;
    };
    const TextureMipRangeUpload range{
        .texture = &texture, .gpu = &output, .firstMip = firstMip, .levelCount = mipLevels - firstMip};
    if (!uploadTextureMipRanges(std::span(&range, 1))) {
        return fail();
    }
    // The copy may still be in flight from here on, so view failures keep the
    // image alive rather than destroying it under the transfer queue.
    if (!createTextureViews(output.image, formats.linear, formats.srgb, firstMip, mipLevels, output.view,
                            output.srgbView)) {
        return false;
    }
    output.residentFirstMip = firstMip;
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return true;
}

bool MeshPass::uploadTextureMipRanges(std::span<const TextureMipRangeUpload> ranges) {
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize stagingBytes = 0;
    for (const TextureMipRangeUpload& range : ranges) {
        for (uint32_t level = range.firstMip; level < range.firstMip + range.levelCount; ++level) {
            const TextureMipLevel& mip = range.texture->mips[level];
            copies.push_back({.bufferOffset = stagingBytes,
                              .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .mipLevel = level,
                                                   .baseArrayLayer = 0,
                                                   .layerCount = 1},
                              .imageExtent = {.width = mip.width, .height = mip.height, .depth = 1}});
            stagingBytes += (mip.size + mipStagingAlignment - 1) / mipStagingAlignment * mipStagingAlignment;
        }
    }
    if (copies.empty()) {
        return true;
    }
    m_mipStagingBytes.resize(static_cast<size_t>(stagingBytes));
    size_t copyIndex = 0;
    for (const TextureMipRangeUpload& range : ranges) {
        for (uint32_t level = range.firstMip; level < range.firstMip + range.levelCount; ++level) {
            const TextureMipLevel& mip = range.texture->mips[level];
            std::memcpy(m_mipStagingBytes.data() + copies[copyIndex++].bufferOffset,
                        range.texture->compressed.data() + mip.offset, static_cast<size_t>(mip.size));
        }
    }
    const VkBuffer staging = m_textureUploadContext.stage(m_mipStagingBytes.data(), stagingBytes);
    if (!staging) {
        return false;
    }

    // Only the uploaded levels change layout. Resident levels keep being
    // sampled through the current views while this runs, and levels above the
    // range stay UNDEFINED until a later range reaches them.
    const auto levelBarriers = [&](VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                   VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                                   VkAccessFlags2 destinationAccess) {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(ranges.size());
        for (const TextureMipRangeUpload& range : ranges) {
            barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = sourceStage,
                                .srcAccessMask = sourceAccess,
                                .dstStageMask = destinationStage,
                                .dstAccessMask = destinationAccess,
                                .oldLayout = oldLayout,
                                .newLayout = newLayout,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = range.gpu->image,
                                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .baseMipLevel = range.firstMip,
                                                     .levelCount = range.levelCount,
                                                     .baseArrayLayer = 0,
                                                     .layerCount = 1}});
        }
        return barriers;
    };
    const auto recordBarriers = [](VkCommandBuffer command, const std::vector<VkImageMemoryBarrier2>& barriers) {
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                          .pImageMemoryBarriers = barriers.data()};
        vkCmdPipelineBarrier2(command, &dependency);
    };
    // Graphics-queue consumers wait on the upload timeline semaphore when the
    // copy runs on a dedicated transfer queue, matching the RGBA8 path.
    const bool dedicatedQueue = m_textureUploadContext.usesDedicatedQueue();
    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        recordBarriers(command, levelBarriers(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                              VK_ACCESS_2_TRANSFER_WRITE_BIT));
        size_t firstCopy = 0;
        for (const TextureMipRangeUpload& range : ranges) {
            vkCmdCopyBufferToImage(command, staging, range.gpu->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   range.levelCount, copies.data() + firstCopy);
            firstCopy += range.levelCount;
        }
        recordBarriers(command,
                       levelBarriers(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     dedicatedQueue ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                    : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     dedicatedQueue ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    });
    if (uploaded) {
        m_uploadGpuMilliseconds += m_textureUploadContext.lastGpuMilliseconds();
    }
    return uploaded;
}

bool MeshPass::createTextureViews(VkImage image, VkFormat linearFormat, VkFormat srgbFormat, uint32_t firstMip,
                                  uint32_t mipCount, VkImageView& view, VkImageView& srgbView) {
    VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = linearFormat,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .baseMipLevel = firstMip,
                                                    .levelCount = mipCount - firstMip,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1}};
    view = VK_NULL_HANDLE;
    srgbView = VK_NULL_HANDLE;
    if (!checkVulkan(vkCreateImageView(m_device, &info, nullptr, &view), "Create block-compressed texture view")) {
        view = VK_NULL_HANDLE;
        return false;
    }
    if (srgbFormat == VK_FORMAT_UNDEFINED) {
        return true;
    }
    info.format = srgbFormat;
    if (!checkVulkan(vkCreateImageView(m_device, &info, nullptr, &srgbView),
                     "Create sRGB block-compressed texture view")) {
        vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
        srgbView = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool MeshPass::publishResidentMips(const TextureAsset& texture, GpuTexture& gpu, uint32_t firstMip) {
    const BlockCompressedFormats formats = blockCompressedFormats(texture);
    VkImageView view = VK_NULL_HANDLE;
    VkImageView srgbView = VK_NULL_HANDLE;
    if (!createTextureViews(gpu.image, formats.linear, formats.srgb, firstMip, gpu.mipCount, view, srgbView)) {
        // The levels stay uploaded but unpublished; a later frame uploads and
        // publishes them again.
        return false;
    }
    // Slots keep their indices, so material records stay valid; each frame
    // set rewrites the slot before it is bound again.
    m_textureTable->replace(gpu.linearSlot, view);
    if (gpu.srgbSlot != gpu.linearSlot) {
        m_textureTable->replace(gpu.srgbSlot, srgbView);
    }
    const VkImageView retiredView = gpu.view;
    const VkImageView retiredSrgbView = gpu.srgbView;
    m_releaseQueue.defer(m_frameSerial, [this, retiredView, retiredSrgbView] {
        vkDestroyImageView(m_device, retiredView, nullptr);
        if (retiredSrgbView) {
            vkDestroyImageView(m_device, retiredSrgbView, nullptr);
        }
    });
    gpu.view = view;
    gpu.srgbView = srgbView;
    gpu.residentFirstMip = firstMip;
    return true;
}

void MeshPass::streamTextureMips(const AssetManager& assets, const glm::vec3& cameraPosition) {
    std::vector<uint64_t> pendingKeys;
    for (const auto& [key, gpu] : m_textures) {
        if (gpu.residentFirstMip > 0) {
            pendingKeys.push_back(key);
        }
    }
    m_textureStreamedBytes = 0;
    if (pendingKeys.empty()) {
        return;
    }
    const auto streamBegin = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, StreamPriority> priorities;
    for (const uint64_t key : pendingKeys) {
        priorities.emplace(key, StreamPriority{});
    }
    for (const RenderItem& item : m_renderItems) {
        if (!item.material) {
            continue;
        }
        const StreamPriority use =
            textureStreamPriority(glm::distance(item.boundsCenter, cameraPosition), item.boundsRadius);
        for (const TextureHandle texture :
             {item.material->baseColorTexture, item.material->normalTexture, item.material->metallicRoughnessTexture,
              item.material->occlusionTexture, item.material->emissiveTexture}) {
            if (const auto found = texture ? priorities.find(handleKey(texture)) : priorities.end();
                found != priorities.end()) {
                found->second = mostDemandingStreamPriority(found->second, use);
            }
        }
    }

    std::vector<std::vector<uint64_t>> levelBytes;
    levelBytes.reserve(pendingKeys.size());
    std::vector<TextureMipStreamCandidate> candidates;
    candidates.reserve(pendingKeys.size());
    for (const uint64_t key : pendingKeys) {
        const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key));
        const GpuTexture& gpu = m_textures.at(key);
        if (!texture || texture->mips.size() != gpu.mipCount) {
            continue;
        }
        std::vector<uint64_t>& bytes = levelBytes.emplace_back();
        for (const TextureMipLevel& mip : texture->mips) {
            bytes.push_back(mip.size);
        }
        candidates.push_back({.key = key,
                              .residency = {.mipCount = gpu.mipCount, .residentFirstMip = gpu.residentFirstMip},
                              .levelBytes = bytes,
                              .priority = priorities.at(key)});
    }
    // Off still finishes textures that were streaming when it was selected.
    const uint64_t budget = textureStreamingActive()
                                ? std::min<uint64_t>(m_textureStreamingBytesPerFrame, maximumMipStreamingBatchBytes)
                                : maximumMipStreamingBatchBytes;
    const std::vector<TextureMipUpload> plan = planTextureMipUploads(candidates, budget);
    std::vector<TextureMipRangeUpload> ranges;
    ranges.reserve(plan.size());
    for (const TextureMipUpload& upload : plan) {
        ranges.push_back({.texture = assets.texture(handleFromKey<TextureHandle>(upload.key)),
                          .gpu = &m_textures.at(upload.key),
                          .firstMip = upload.firstMip,
                          .levelCount = upload.levelCount});
    }
    // Staging backpressure leaves everything as it was; the plan reruns next frame.
    if (!ranges.empty() && uploadTextureMipRanges(ranges)) {
        for (size_t index = 0; index < ranges.size(); ++index) {
            if (publishResidentMips(*ranges[index].texture, *ranges[index].gpu, ranges[index].firstMip)) {
                m_textureStreamedBytes += plan[index].bytes;
            }
        }
    }
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamBegin).count();
}

bool MeshPass::completeTextureMips(uint64_t key, const AssetManager& assets) {
    GpuTexture& gpu = m_textures.at(key);
    const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key));
    if (gpu.residentFirstMip == 0) {
        return true;
    }
    if (!texture || texture->mips.size() != gpu.mipCount) {
        return false;
    }
    const TextureMipRangeUpload range{
        .texture = texture, .gpu = &gpu, .firstMip = 0, .levelCount = gpu.residentFirstMip};
    return uploadTextureMipRanges(std::span(&range, 1)) && publishResidentMips(*texture, gpu, 0);
}

void MeshPass::recordTextureStreaming(const AssetManager& assets, RenderStats& stats) const {
    stats.textureStreamedBytes = m_textureStreamedBytes;
    for (const auto& [key, gpu] : m_textures) {
        stats.textureResidentMips += gpu.mipCount - gpu.residentFirstMip;
        stats.textureRequestedMips += gpu.mipCount;
        if (gpu.residentFirstMip == 0) {
            continue;
        }
        ++stats.texturesStreaming;
        if (const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key))) {
            for (uint32_t level = 0; level < gpu.residentFirstMip && level < texture->mips.size(); ++level) {
                stats.textureStreamingBacklogBytes += texture->mips[level].size;
            }
        }
    }
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
        m_log.error("Mesh vertices cannot be packed for upload: " + packError);
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
        m_log.error("Mesh rejected: " + std::to_string(stagingBytes) + " bytes exceed the " +
                    std::to_string(UploadContext::maximumSlotBytes) + "-byte upload limit");
        m_unpackableMeshes.insert(key);
        return nullptr;
    }
    const auto geometry =
        m_geometry->allocate(static_cast<uint32_t>(mesh->vertices.size()), static_cast<uint32_t>(mesh->indices.size()),
                             streams->skinned() ? GeometryLayout::Skinned : GeometryLayout::Static);
    if (!geometry) {
        // Retried every frame while the arena is full; logged once per episode.
        if (!m_geometryArenaFullReported) {
            m_log.warning("Geometry arena could not place a mesh; its draws are deferred until space frees");
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
        m_log.warning("Texture rejected: " + reason + "; materials sample a fallback texture");
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
            m_log.warning("Bindless texture table is full (" + std::to_string(m_textureTable->capacity()) +
                          " slots); textures are deferred until slots free");
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
        m_log.error("Material count exceeds the renderer capacity (" + std::to_string(maximumMaterials) + ")");
        return nullptr;
    }
    // Upload referenced textures now so frame recording only resolves slots.
    for (const TextureHandle texture : material->textures()) {
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
        records[gpu.index] = packMaterialRecord(
            *material, {.baseColor = slotFor(material->baseColorTexture, true, m_whiteSrgbSlot),
                        .normal = slotFor(material->normalTexture, false, m_normalSlot),
                        .metallicRoughness = slotFor(material->metallicRoughnessTexture, false, m_whiteSlot),
                        .occlusion = slotFor(material->occlusionTexture, false, m_whiteSlot),
                        .emissive = slotFor(material->emissiveTexture, true, m_whiteSrgbSlot),
                        .clearcoat = slotFor(material->clearcoat.texture, false, m_whiteSlot),
                        .clearcoatRoughness = slotFor(material->clearcoat.roughnessTexture, false, m_whiteSlot),
                        .clearcoatNormal = slotFor(material->clearcoat.normalTexture, false, m_normalSlot),
                        .sheenColor = slotFor(material->sheen.colorTexture, true, m_whiteSrgbSlot),
                        .sheenRoughness = slotFor(material->sheen.roughnessTexture, false, m_whiteSlot),
                        .specular = slotFor(material->specular.texture, false, m_whiteSlot),
                        .specularColor = slotFor(material->specular.colorTexture, true, m_whiteSrgbSlot),
                        .transmission = slotFor(material->transmission.texture, false, m_whiteSlot)});
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
                              SceneAttachments attachments, bool depthInitialized) const {
    const bool clear = attachments == SceneAttachments::Clear;
    // A preserved pass continues one the caller already transitioned.
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage, depthInitialized);
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

void MeshPass::prepareDepthForClear(VkCommandBuffer commandBuffer, VkImage depthImage, bool depthInitialized) const {
    // Last frame's post-processing sampled the depth it stored.
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 depthInitialized ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 depthInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 depthInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void MeshPass::beginDepthPrepass(VkCommandBuffer commandBuffer, VkImageView normalView, VkImageView specularView,
                                 VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                                 SceneAttachments attachments, bool depthInitialized) const {
    const bool clear = attachments == SceneAttachments::Clear;
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage, depthInitialized);
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

std::vector<int32_t> MeshPass::recordLocalShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                                  const Scene& scene, const AssetManager& assets,
                                                  const RenderSettings& settings, std::span<const LocalLight> lights,
                                                  const glm::vec3& cameraPosition, OptionalPassTimestamps& timestamps,
                                                  RenderStats& stats) {
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
    m_localShadowRevisions = revisions;
    m_localShadowAssetRevision = assets.revision();
    stats.cpuLocalShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuBegin).count();
    return firstTiles;
}

void MeshPass::collectShadowCasterChanges(const Scene& scene) {
    m_changedShadowCasters.clear();
    const auto addCaster = [&](const RenderItem& item) {
        if (item.material->translucent()) {
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
        if (!item.material->translucent() && sphereReachesClipVolume(frustum, item.boundsCenter, item.boundsRadius)) {
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
            if (light.shadows.enabled && !item.material->translucent() &&
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

struct MeshPass::FrameState {
    // Camera data every view starts from: the sun, the environment, the
    // irradiance volume, and the shadow cascades the leading view placed.
    CameraData base;
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

namespace {
// Per-view counters a view's recording accumulates into the frame's stats.
void addViewCounters(RenderStats& frame, const RenderStats& view) {
    frame.visibleCount += view.visibleCount;
    frame.culledCount += view.culledCount;
    frame.drawCalls += view.drawCalls;
    frame.triangleCount += view.triangleCount;
    frame.indirectCommandCount += view.indirectCommandCount;
    frame.gpuVisibilityMismatches += view.gpuVisibilityMismatches;
    frame.temporalHistoryBytes += view.temporalHistoryBytes;
    frame.spatialUpscaleBytes += view.spatialUpscaleBytes;
    frame.motionVectorBytes += view.motionVectorBytes;
    frame.ambientOcclusionBytes += view.ambientOcclusionBytes;
    frame.reflectionBytes += view.reflectionBytes;
    frame.volumetricFogBytes += view.volumetricFogBytes;
    frame.depthOfFieldBytes += view.depthOfFieldBytes;
    frame.motionBlurBytes += view.motionBlurBytes;
    frame.decalBytes += view.decalBytes;
    frame.pickPasses += view.pickPasses;
}
} // namespace

MeshPass::ViewFrame MeshPass::prepareViewFrame(ViewState& view, const MeshPassViewRequest& request, const Scene& scene,
                                               double previousGpuFrameMilliseconds) const {
    const RenderSettings& settings = *request.settings;
    ViewFrame frame;
    frame.verticalFov = glm::radians(60.0f);
    auto cameras = scene.registry().view<const CameraComponent, const TransformComponent>();
    for (Entity entity : cameras) {
        const auto& camera = cameras.get<const CameraComponent>(entity);
        if (camera.primary) {
            frame.view = glm::inverse(cameras.get<const TransformComponent>(entity).world);
            frame.verticalFov = camera.verticalFovRadians;
            frame.nearPlane = camera.nearPlane;
            frame.farPlane = camera.farPlane;
            break;
        }
    }
    if (request.cameraOverride) {
        const RenderView& override = *request.cameraOverride;
        frame.view = glm::lookAtRH(override.position, override.position + override.forward, override.up);
        frame.verticalFov = override.verticalFovRadians;
        frame.nearPlane = override.nearPlane;
        frame.farPlane = override.farPlane;
    }
    const glm::mat4 inverseView = glm::inverse(frame.view);
    frame.cameraPosition = glm::vec3(inverseView[3]);
    frame.cameraForward = -glm::normalize(glm::vec3(inverseView[2]));
    frame.cameraCut = !view.cameraHistoryValid ||
                      glm::distance(frame.cameraPosition, view.previousCameraPosition) > 10.0f ||
                      glm::dot(frame.cameraForward, view.previousCameraForward) < 0.5f;
    // An upscaler lowers the internal resolution to its preset's scale. FSR 1
    // without support composites at output resolution (PostProcessPass), so
    // it renders natively rather than stretching a reduced frame.
    UpscalerSettings upscaler = settings.upscaler;
    if (upscaler.mode == UpscalerMode::Fsr1 && !view.postProcess.spatialUpscaleSupported()) {
        upscaler.mode = UpscalerMode::Off;
    }
    const bool temporalAntialiasing = settings.antialiasing == AntialiasingMode::Taa;
    frame.temporal = view.temporalQuality.beginFrame(
        request.extent.width, request.extent.height, previousGpuFrameMilliseconds,
        upscaledDynamicResolution(settings.dynamicResolution, upscaler), temporalAntialiasing, frame.cameraCut);
    frame.renderExtent = {frame.temporal.internalWidth, frame.temporal.internalHeight};
    // Swapping the planes yields reversed Z; see DepthConvention.hpp.
    glm::mat4 projection = glm::perspectiveRH_ZO(
        frame.verticalFov, static_cast<float>(frame.renderExtent.width) / static_cast<float>(frame.renderExtent.height),
        frame.farPlane, frame.nearPlane);
    projection[1][1] *= -1.0f;
    frame.unjitteredViewProjection = projection * frame.view;
    if (temporalAntialiasing) {
        projection = applyProjectionJitter(projection, frame.temporal.jitterNdc);
    }
    frame.projection = projection;
    frame.lodView = makeMeshLodView(frame.cameraPosition, projection[1][1], frame.renderExtent.height,
                                    settings.lodErrorThresholdPixels);
    return frame;
}

bool MeshPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                      const AssetManager& assets, RenderRuntime& rendering, std::span<const MeshPassViewRequest> views,
                      FrameTaskExecutor& frameTasks, VkQueryPool timestampQueries) {
    if (frameIndex >= m_cameras.size()) {
        return false;
    }
    if (views.empty()) {
        m_log.error("A frame must record at least one render view");
        return false;
    }
    for (size_t index = 0; index < views.size(); ++index) {
        const MeshPassViewRequest& request = views[index];
        const bool duplicate =
            std::any_of(views.begin(), views.begin() + static_cast<std::ptrdiff_t>(index),
                        [&](const MeshPassViewRequest& earlier) { return earlier.view == request.view; });
        if (!hasView(request.view) || duplicate || request.settings == nullptr) {
            m_log.error("Render view request " + std::to_string(index) + " names view slot " +
                        std::to_string(request.view) + ", which is missing, repeated, or has no settings");
            return false;
        }
        if (request.view != 0 && request.plan.gpuDrivenSubmission) {
            m_log.error("Only the default render view may use GPU-driven submission");
            return false;
        }
    }

    const double previousGpuFrameMilliseconds = rendering.m_stats.gpuFrameMilliseconds;
    rendering.m_stats = {};

    // The caller waited for this frame slot's fence, so every frame at least
    // one full ring older than this one has completed on the GPU.
    ++m_frameSerial;
    if (m_frameSerial > m_cameras.size()) {
        const uint64_t completedSerial = m_frameSerial - m_cameras.size();
        m_releaseQueue.collect(completedSerial);
        m_particles.collect(completedSerial);
        m_idBuffer.collect(completedSerial);
        for (const std::unique_ptr<ViewState>& view : m_views) {
            if (view) {
                view->volumetricFog.collect(completedSerial);
            }
        }
    }
    pruneUnusedAssets(assets);
    m_textureStreamingBytesPerFrame = rendering.settings().textureStreamingBytesPerFrame;

    // Render packets mark the union of every view's outlines; views filter.
    m_outlinedObjects.clear();
    for (const MeshPassViewRequest& request : views) {
        m_outlinedObjects.insert(m_outlinedObjects.end(), request.outlinedObjects.begin(),
                                 request.outlinedObjects.end());
    }
    if (views.size() > 1) {
        std::ranges::sort(m_outlinedObjects);
        const auto duplicates = std::ranges::unique(m_outlinedObjects);
        m_outlinedObjects.erase(duplicates.begin(), duplicates.end());
    }

    // Fog volumes follow each view's quality. Their descriptors are written
    // before anything in this command buffer binds the camera sets, since the
    // default view's sets are the frame's.
    for (const MeshPassViewRequest& request : views) {
        ViewState& view = *m_views[request.view];
        if (!view.volumetricFog.prepare(request.settings->volumetricFog, m_frameSerial)) {
            return false;
        }
        writeVolumetricFogDescriptors(view, frameIndex);
    }
    OptionalPassTimestamps passTimestamps(commandBuffer, timestampQueries);
    const ViewFrame leadFrame =
        prepareViewFrame(*m_views[views.front().view], views.front(), scene, previousGpuFrameMilliseconds);
    FrameState frame;
    if (!recordFrameStage(commandBuffer, frameIndex, scene, assets, rendering, views.front(), leadFrame, frameTasks,
                          timestampQueries, passTimestamps, frame)) {
        return false;
    }
    // Views after the leading one write no timestamps: each query is written
    // once per frame.
    OptionalPassTimestamps noTimestamps(commandBuffer, VK_NULL_HANDLE);
    for (size_t index = 0; index < views.size(); ++index) {
        const bool leading = index == 0;
        const MeshPassViewRequest& request = views[index];
        const ViewFrame viewFrame =
            leading ? leadFrame
                    : prepareViewFrame(*m_views[request.view], request, scene, previousGpuFrameMilliseconds);
        if (!recordView(commandBuffer, frameIndex, scene, assets, rendering, request, viewFrame, leading, frameTasks,
                        leading ? timestampQueries : VK_NULL_HANDLE, leading ? passTimestamps : noTimestamps, frame)) {
            return false;
        }
    }
    if (std::ranges::none_of(views, [](const MeshPassViewRequest& request) { return request.view == 0; })) {
        // The shadow passes read the cascades from the frame's camera buffer,
        // which only the default view writes otherwise.
        std::memcpy(m_cameras[frameIndex].mapped, &frame.base, sizeof(frame.base));
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].buffer.allocation, 0, sizeof(frame.base));
    }
    // After post-processing, whose optional passes (depth of field, motion
    // blur, colour grading) also write through passTimestamps.
    passTimestamps.closeSkippedPasses();
    for (RenderItem& item : m_renderItems) {
        item.previousWorld = item.world;
        item.previousJointPalette = item.jointPalette;
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 4);
    }
    return true;
}

bool MeshPass::recordFrameStage(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                                const AssetManager& assets, RenderRuntime& rendering, const MeshPassViewRequest& lead,
                                const ViewFrame& leadFrame, FrameTaskExecutor& frameTasks, VkQueryPool timestampQueries,
                                OptionalPassTimestamps& passTimestamps, FrameState& frame) {
    const RenderSettings& settings = rendering.settings();
    RenderStats& stats = rendering.m_stats;
    ViewState& leadView = *m_views[lead.view];
    frame.light = resolvePrimary<DirectionalLightComponent>(scene);
    frame.environment = resolvePrimary<EnvironmentLightComponent>(scene);
    frame.fogMedium = primaryVolumetricFog(scene);
    const DirectionalLightComponent& light = frame.light;
    const EnvironmentLightComponent& environment = frame.environment;
    // Hemisphere lighting reads these flat colors; image-based lighting reads
    // the maps EnvironmentPass binds.
    std::array<glm::vec3, 6> environmentSpecular;
    environmentSpecular.fill((environment.skyColor + environment.groundColor) * 0.5f);
    frame.lightDirection = normalizedLightDirection(light.direction);
    const glm::vec3 lightDirection = frame.lightDirection;
    CameraData& cameraData = frame.base;
    const float shadowDepthRange = std::max(light.shadows.depthRange, 0.2f);
    uint32_t cascadeCount = 0;
    switch (settings.shadows) {
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
    frame.cascadeCount = cascadeCount;
    // The cascades follow the leading view's camera.
    const float nearPlane = leadFrame.nearPlane;
    const float farPlane = leadFrame.farPlane;
    const glm::vec3& cameraPosition = leadFrame.cameraPosition;
    const glm::vec3& cameraForward = leadFrame.cameraForward;
    const float maximumShadowDistance = std::max(std::min(farPlane, shadowDepthRange), nearPlane + 0.01f);
    const glm::vec3 lightUp =
        std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    float cascadeNear = nearPlane;
    const float aspect = static_cast<float>(lead.extent.width) / static_cast<float>(lead.extent.height);
    for (uint32_t cascade = 0; cascade < std::max(cascadeCount, 1u); ++cascade) {
        float cascadeFar = maximumShadowDistance;
        float radius = std::max(light.shadows.halfExtent, 0.1f);
        glm::vec3 center = cameraPosition + cameraForward * std::max(light.shadows.focusDistance, 0.0f);
        if (cascadeCount > 1) {
            const float partition = static_cast<float>(cascade + 1) / static_cast<float>(cascadeCount);
            const float logarithmic = nearPlane * std::pow(maximumShadowDistance / nearPlane, partition);
            const float uniform = nearPlane + (maximumShadowDistance - nearPlane) * partition;
            const float lambda = std::clamp(settings.cascadeSplitLambda, 0.0f, 1.0f);
            cascadeFar = glm::mix(uniform, logarithmic, lambda);
            const float halfHeight = std::tan(leadFrame.verticalFov * 0.5f) * cascadeFar;
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
    frame.shadowsEnabled = light.shadows.enabled && settings.shadows != ShadowQuality::Off;
    cameraData.shadowSettings = {std::max(light.shadows.normalBias, 0.0f), frame.shadowsEnabled ? 1.0f : 0.0f,
                                 static_cast<float>(cascadeCount),
                                 std::clamp(settings.cascadeBlendFraction, 0.0f, 0.5f)};
    for (size_t level = 0; level < environmentSpecular.size(); ++level) {
        cameraData.environmentSpecular[level] = {glm::max(environmentSpecular[level], glm::vec3(0.0f)), 1.0f};
    }
    // Environment maps are bound before any pass uses this frame's camera sets.
    frame.imageBasedLighting = settings.environment != EnvironmentQuality::Hemisphere;
    const CameraBuffer& frameCamera = m_cameras[frameIndex];
    const std::array<VkDescriptorSet, 2> environmentSets{frameCamera.descriptor, frameCamera.gpuDrivenDescriptor};
    std::vector<EnvironmentSource> residentProbeSources;
    // Probe residency is frame-level, so every view shares the lead's choice.
    frame.residentProbes = lead.plan.perPixelReflectionProbes
                               ? selectPerPixelProbes(scene, assets, environment, cameraPosition, residentProbeSources)
                               : std::vector<ReflectionProbeVolume>{};
    passTimestamps.write(FrameTimestamp::environmentPrecomputeBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const std::optional<EnvironmentFrameState> environmentState = m_environmentPass.prepare(
        commandBuffer, frameIndex, m_frameSerial,
        environmentRequest(scene, assets, environment, settings, cameraPosition, lead.plan.perPixelReflectionProbes,
                           std::move(residentProbeSources)),
        std::span(environmentSets.data(), frameCamera.gpuDrivenDescriptor ? 2u : 1u));
    passTimestamps.write(FrameTimestamp::environmentPrecomputeEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (!environmentState) {
        return false;
    }
    frame.environmentState = *environmentState;
    // Irradiance volume (U6): bind the volume nearest the leading camera and
    // upload its probes when they change; Environment mode leaves it disabled.
    passTimestamps.write(FrameTimestamp::irradianceVolumeBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const Entity irradianceVolumeEntity = settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume
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
    frame.localLightLimit = settings.localLights == LocalLightingQuality::Disabled
                                ? 0u
                                : std::min(settings.maximumLocalLights, leadView.clusteredLighting.lightCapacity());

    uint64_t outlineRevision = 0;
    for (RenderObjectId outlined : m_outlinedObjects) {
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
        if (!buildRenderItems(scene, assets, m_outlinedObjects, m_renderItems)) {
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
    streamTextureMips(assets, cameraPosition);
    updateBindlessDescriptor(frameIndex);
    const auto packetBuildEnd = std::chrono::steady_clock::now();
    const double packetBuildMilliseconds =
        std::chrono::duration<double, std::milli>(packetBuildEnd - packetBuildStart).count();
    if (m_renderItems.size() > maximumObjectsPerFrame) {
        m_log.error("Renderable count exceeds the per-frame render capacity");
        return false;
    }
    const FrameResourceUse lightResources[]{{"scene.lights", FrameResourceAccess::Read},
                                            {"frame.light_list", FrameResourceAccess::Write}};
    const FrameResourceUse visibilityResources[]{{"renderer.persistent_scene", FrameResourceAccess::Read},
                                                 {"frame.visibility", FrameResourceAccess::Write}};
    FrameTaskGraph taskGraph(++m_taskGeneration);
    taskGraph.add("light-selection", lightResources, [&](const FrameTaskContext&) {
        if (frame.localLightLimit > 0) {
            frame.sceneLights = gatherLocalLights(scene);
            frame.leadLights = selectLocalLights(frame.sceneLights, cameraPosition, frame.localLightLimit);
        }
    });
    const glm::mat4 leadViewProjection = leadFrame.projection * leadFrame.view;
    taskGraph.add("render-visibility", visibilityResources, [&](const FrameTaskContext&) {
        m_gpuScene.buildVisibility({.viewProjection = leadViewProjection, .depthHistoryValid = false},
                                   leadView.visibility);
    });
    const FrameTaskResult taskResult = frameTasks.execute(taskGraph);
    if (!taskResult) {
        m_log.error("Frame task graph failed: " + taskResult.error);
        return false;
    }
    m_gpuScene.retireDeltas();
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
    recordDeferredUploads(settings, stats);
    recordTextureStreaming(assets, stats);
    stats.taskQueueDepth = taskResult.stats.maximumQueueDepth;
    stats.taskStolenJobs = taskResult.stats.stolenJobs;
    stats.taskWorkerUtilization = taskResult.stats.workerUtilization;
    stats.taskMainThreadAssistMilliseconds = taskResult.stats.mainThreadAssistMilliseconds;
    stats.taskGraphLatencyMilliseconds = taskResult.stats.latencyMilliseconds;
    stats.renderableCount = static_cast<uint32_t>(m_renderItems.size());
    stats.particleBytes = m_particles.allocatedBytes();
    stats.irradianceVolumeBytes = m_irradianceVolume.allocatedBytes();
    stats.irradianceVolumeProbes =
        irradianceVolume ? static_cast<uint32_t>(irradianceVolume->bakedData->probes.size()) : 0u;
    stats.shadowCacheBytes =
        static_cast<uint64_t>(shadowResolution) * shadowResolution * sizeof(float) * maximumShadowCascades;
    DirectionalLightComponent shadowLight = light;
    shadowLight.shadows.enabled = frame.shadowsEnabled;
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
            m_log.error("Skin palettes exceed the per-frame render capacity");
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
    // Shadow cascades reuse the leading view's LOD choice.
    selectRenderItemLods(leadFrame.lodView);
    // Local shadows may rebind the atlas in the camera sets, so they run before
    // GPU-driven preparation and before anything binds those sets.
    const size_t shadowedLightCount =
        std::min<size_t>(frame.leadLights.size(), leadView.clusteredLighting.lightCapacity());
    frame.leadShadowTiles = recordLocalShadows(commandBuffer, frameIndex, scene, assets, settings,
                                               std::span<const LocalLight>(frame.leadLights.data(), shadowedLightCount),
                                               cameraPosition, passTimestamps, stats);
    const bool gpuDrivenLead = lead.view == 0 && m_gpuDrivenSupported && lead.plan.gpuDrivenSubmission;
    if (gpuDrivenLead) {
        // Shadow cascades cull from the same records before the camera does.
        if (!prepareGpuDrivenFrame(frameIndex, leadFrame.lodView,
                                   static_cast<uint32_t>(leadView.visibility.visibleInstances.size()))) {
            return false;
        }
        m_gpuDrivenPass.beginFrame(commandBuffer, frameIndex);
        // Every cull dispatch statically references the pyramid.
        leadView.depthPyramid.prepareFirstUse(commandBuffer);
        frame.gpuDrivenPrepared = true;
    }
    // Cascades cull on the GPU whenever the leading view uses GPU-driven
    // submission; the CPU path is the fallback and the A/B baseline
    // (docs/RENDERER_VALIDATION.md).
    frame.shadowCulling = gpuDrivenLead && lead.plan.gpuShadowCulling ? ShadowCulling::Gpu : ShadowCulling::Cpu;
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
                      cascadeCount, dirtyCascades, frame.shadowCulling, leadFrame.lodView, passTimestamps, stats)) {
        return false;
    }
    m_shadowRevisions = revisions;
    m_cachedLightViewProjections = cameraData.lightViewProjections;
    m_cachedCascadeCount = cascadeCount;
    stats.cpuShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowCpuBegin).count();
    return true;
}

bool MeshPass::recordView(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                          const AssetManager& assets, RenderRuntime& rendering, const MeshPassViewRequest& request,
                          const ViewFrame& viewFrame, bool leading, FrameTaskExecutor& frameTasks,
                          VkQueryPool timestampQueries, OptionalPassTimestamps& passTimestamps, FrameState& frame) {
    const auto viewCpuBegin = std::chrono::steady_clock::now();
    ViewState& view = *m_views[request.view];
    const RenderSettings& settings = *request.settings;
    const MeshPassFramePlan& plan = request.plan;
    const VkExtent2D extent = request.extent;
    const VkExtent2D renderExtent = viewFrame.renderExtent;
    const TemporalFrameState& temporal = viewFrame.temporal;
    const glm::mat4& viewMatrix = viewFrame.view;
    const glm::mat4& projection = viewFrame.projection;
    const glm::vec3& cameraPosition = viewFrame.cameraPosition;
    // Counters of this view; merged into the frame's stats at the end.
    RenderStats stats;
    RenderStats& frameStats = rendering.m_stats;
    // Opaque and masked surfaces go through GPU culling; blended surfaces and
    // editor outlines keep CPU submission within the same frame.
    const bool gpuDrivenActive = view.sharesFrameSets && m_gpuDrivenSupported && plan.gpuDrivenSubmission;
    const bool depthPrepass = plan.depthPrepass;
    const MeshPipelineSet* shadingPipelines =
        meshPipelines({.physicallyBased = settings.lighting == LightingQuality::PhysicallyBased,
                       .shadowCascades = frame.cascadeCount,
                       .depthPrepass = depthPrepass,
                       .solidSurfaces = usesSolidSurfaceShading(settings.viewShading),
                       .wireframeOverlay = drawsWireframeOverlay(settings.viewShading)});
    if (!shadingPipelines) {
        return false;
    }
    CameraData cameraData = frame.base;
    cameraData.viewProjection = projection * viewMatrix;
    cameraData.previousViewProjection = view.previousViewProjection;
    cameraData.temporalJitter = {temporal.jitterNdc, view.previousJitterNdc};
    cameraData.cameraPosition = {cameraPosition, 1.0f};
    // View shading: every mode but Rendered replaces the scene's lights.
    if (usesStudioLighting(settings.viewShading)) {
        applyStudioLighting(cameraData, studioLighting(glm::inverse(viewMatrix)));
    }
    cameraData.renderingSettings = {
        settings.lighting == LightingQuality::PhysicallyBased ? 1.0f : 0.0f,
        1.0f / static_cast<float>(extent.width),
        1.0f / static_cast<float>(extent.height),
        settings.visualizeShadowCascades ? 1.0f : 0.0f,
    };
    // Probes still waiting for their maps to build are left out this frame.
    std::vector<GpuReflectionProbe> probeRecords;
    const std::vector<int32_t>& probeSlots = frame.environmentState.probeSlots;
    for (size_t index = 0; index < frame.residentProbes.size() && index < probeSlots.size(); ++index) {
        if (const int32_t slot = probeSlots[index]; slot >= 0) {
            probeRecords.push_back(
                packReflectionProbe(frame.residentProbes[index], static_cast<uint32_t>(slot), viewMatrix));
        }
    }
    cameraData.reflectionProbeParams = {probeRecords.empty() ? 0.0f : 1.0f, static_cast<float>(probeRecords.size()),
                                        0.0f, 0.0f};
    // GTAO needs the prepass normals; without them the occlusion target is
    // cleared to unoccluded and any scale samples it correctly.
    const AmbientOcclusionQuality ambientOcclusion = plan.ambientOcclusion;
    cameraData.environmentParams = {frame.imageBasedLighting ? 1.0f : 0.0f, frame.environmentState.probeWeight,
                                    frame.environmentState.specularMaximumLod,
                                    AmbientOcclusionPass::resolutionScale(ambientOcclusion)};
    // The leading view's lights and visibility came from the frame stage.
    std::vector<LocalLight> viewLights;
    if (!leading) {
        const FrameResourceUse lightResources[]{{"frame.lights", FrameResourceAccess::Read},
                                                {"view.light_list", FrameResourceAccess::Write}};
        const FrameResourceUse visibilityResources[]{{"renderer.persistent_scene", FrameResourceAccess::Read},
                                                     {"view.visibility", FrameResourceAccess::Write}};
        FrameTaskGraph taskGraph(++m_taskGeneration);
        taskGraph.add("view-light-selection", lightResources, [&](const FrameTaskContext&) {
            if (frame.localLightLimit > 0) {
                viewLights = selectLocalLights(frame.sceneLights, cameraPosition, frame.localLightLimit);
            }
        });
        taskGraph.add("view-visibility", visibilityResources, [&](const FrameTaskContext&) {
            m_gpuScene.buildVisibility({.viewProjection = cameraData.viewProjection, .depthHistoryValid = false},
                                       view.visibility);
        });
        const FrameTaskResult taskResult = frameTasks.execute(taskGraph);
        if (!taskResult) {
            m_log.error("View task graph failed: " + taskResult.error);
            return false;
        }
    }
    const std::vector<LocalLight>& lights = leading ? frame.leadLights : viewLights;
    ClusterLightStatistics clusterStatistics;
    const uint32_t localLightCount = view.clusteredLighting.beginFrame(frameIndex, lights, clusterStatistics);
    // Only the leading view's lights had shadow tiles rendered; another view
    // finds a light's tiles by its stable id.
    view.clusteredLighting.assignShadowTiles(
        frameIndex,
        leading ? frame.leadShadowTiles : remapLocalShadowTiles(frame.leadLights, frame.leadShadowTiles, lights));
    uint32_t sceneDecalCount = 0;
    const std::vector<GpuDecal> decalRecords =
        prepareDecals(scene, assets, settings, cameraPosition, viewMatrix, sceneDecalCount);
    DecalClusterStatistics decalStatistics;
    const uint32_t decalCount = view.decalClusters.beginFrame(frameIndex, decalRecords, decalStatistics);
    const ClusterGrid clusterGrid =
        makeClusterGrid(view.clusteredLighting.configuration(), projection, {renderExtent.width, renderExtent.height},
                        viewFrame.nearPlane, viewFrame.farPlane);
    const glm::vec2 sliceScaleBias = clusterGrid.sliceScaleBias();
    cameraData.clusterDepth = {sliceScaleBias, projection[3][2], projection[2][2]};
    cameraData.clusterTileScale = {glm::vec2(clusterGrid.configuration.columns, clusterGrid.configuration.rows) /
                                       clusterGrid.renderExtent,
                                   0.0f, 0.0f};
    cameraData.clusterGrid = {clusterGrid.configuration.columns, clusterGrid.configuration.rows,
                              clusterGrid.configuration.depthSlices, localLightCount};
    // The fog volumes were prepared before the frame stage (see record()); the
    // scene decides whether there is a medium to draw at all.
    const bool fogActive = settings.volumetricFog != VolumetricFogQuality::Off && view.volumetricFog.supported() &&
                           frame.fogMedium && frame.fogMedium->density > 0.0f;
    const volumetric_fog::FroxelGrid froxelGrid =
        volumetric_fog::makeFroxelGrid(clusterGrid, view.volumetricFog.subdivision());
    if (fogActive) {
        cameraData.fogVolume = {1.0f, froxelGrid.sliceScaleBias(), static_cast<float>(froxelGrid.dimensions().z)};
    }
    // Written every frame: transmissive surfaces map fragments to the
    // transmission background with it too.
    cameraData.fogScreen = {1.0f / static_cast<float>(renderExtent.width),
                            1.0f / static_cast<float>(renderExtent.height), 0.0f, 0.0f};
    if (view.sharesFrameSets) {
        std::memcpy(m_cameras[frameIndex].mapped, &cameraData, sizeof(cameraData));
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].buffer.allocation, 0, sizeof(cameraData));
    } else {
        ViewCameraSet& camera = view.cameras[frameIndex];
        std::memcpy(camera.uniformMapped, &cameraData, sizeof(cameraData));
        vmaFlushAllocation(m_allocator, camera.uniform.allocation, 0, sizeof(cameraData));
        copySharedCameraBindings(view, frameIndex);
    }
    if (leading) {
        frameStats.localLightCount = localLightCount;
        frameStats.reflectionProbeCount = static_cast<uint32_t>(probeRecords.size());
        frameStats.discardedLightAssignments = clusterStatistics.discardedAssignments;
        frameStats.maximumLightsPerCluster = clusterStatistics.maximumLightsInCluster;
        frameStats.droppedLocalLights = frame.sceneLights.size() > localLightCount
                                            ? static_cast<uint32_t>(frame.sceneLights.size() - localLightCount)
                                            : 0u;
        frameStats.decalCount = decalCount;
        frameStats.droppedDecals = sceneDecalCount > decalCount ? sceneDecalCount - decalCount : 0u;
        frameStats.discardedDecalAssignments = decalStatistics.discardedAssignments;
        frameStats.maximumDecalsPerCluster = decalStatistics.maximumDecalsInCluster;
        frameStats.internalRenderWidth = renderExtent.width;
        frameStats.internalRenderHeight = renderExtent.height;
        frameStats.internalResolutionScale = temporal.resolutionScale;
        frameStats.temporalHistoryValid = temporal.historyValid;
        frameStats.temporalHistoryInvalidated = temporal.historyInvalidated;
    }
    stats.decalBytes = view.decalClusters.allocatedBytes();
    stats.temporalHistoryBytes = view.postProcess.temporalHistoryBytes();
    stats.spatialUpscaleBytes = view.postProcess.spatialUpscaleBytes();
    stats.motionVectorBytes =
        static_cast<uint64_t>(extent.width) * extent.height * sizeof(uint16_t) * 4 * m_cameras.size();
    stats.ambientOcclusionBytes = view.ambientOcclusion.allocatedBytes();
    stats.volumetricFogBytes = view.volumetricFog.allocatedBytes();
    if (!leading) {
        // The frame stage selected the leading view's LODs.
        selectRenderItemLods(viewFrame.lodView);
    }
    const DrawContext draw{.commandBuffer = commandBuffer,
                           .frameIndex = frameIndex,
                           .pipelines = shadingPipelines,
                           .timestampQueries = timestampQueries,
                           .passTimestamps = &passTimestamps,
                           .stats = &stats,
                           .view = viewMatrix,
                           .renderExtent = renderExtent,
                           .viewState = &view,
                           .cameraSet = viewCameraSet(view, frameIndex),
                           .objectBase = view.sharesFrameSets ? mainObjectBase : 0u};
    if (gpuDrivenActive) {
        if (!frame.gpuDrivenPrepared) {
            // The default view does not lead this frame, so its records were
            // not prepared before the shadows.
            if (!prepareGpuDrivenFrame(frameIndex, viewFrame.lodView,
                                       static_cast<uint32_t>(view.visibility.visibleInstances.size()))) {
                return false;
            }
            m_gpuDrivenPass.beginFrame(commandBuffer, frameIndex);
            view.depthPyramid.prepareFirstUse(commandBuffer);
            frame.gpuDrivenPrepared = true;
        }
        // Culling time covers the early phase; the depth pyramid and late phase
        // run between the opaque passes and count toward opaque time.
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 8);
        }
        m_gpuDrivenPass.cull(commandBuffer, frameIndex, GpuCullPhase::Early, cameraData.viewProjection,
                             view.depthPyramid, viewFrame.lodView);
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 9);
        }
    } else if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 8);
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestampQueries, 9);
    }
    view.previousCameraPosition = cameraPosition;
    view.previousCameraForward = viewFrame.cameraForward;
    view.cameraHistoryValid = true;
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 1);
    }
    if (!collectCpuSubmittedItems(view, gpuDrivenActive, cameraPosition, request.outlinedObjects)) {
        return false;
    }
    uploadCpuSubmittedObjects(draw);
    if (!request.picks.empty() && !recordPicks(draw, scene, request, viewFrame)) {
        return false;
    }
    stats.culledCount += view.visibility.frustumCulled + (gpuDrivenActive ? 0u : view.visibility.occlusionCulled);

    passTimestamps.write(FrameTimestamp::lightClustersBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    view.clusteredLighting.record(commandBuffer, frameIndex, clusterGrid, viewMatrix);
    passTimestamps.write(FrameTimestamp::lightClustersEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    if (!probeRecords.empty()) {
        passTimestamps.write(FrameTimestamp::reflectionProbeClustersBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        view.reflectionProbeClusters.record(commandBuffer, frameIndex, probeRecords,
                                            m_environmentPass.probeIrradianceBuffer(), clusterGrid);
        passTimestamps.write(FrameTimestamp::reflectionProbeClustersEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    if (decalCount > 0) {
        passTimestamps.write(FrameTimestamp::decalClustersBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        view.decalClusters.record(commandBuffer, frameIndex, clusterGrid);
        passTimestamps.write(FrameTimestamp::decalClustersEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    const ClusteredLightingPass::FrameBuffers clusterBuffers = view.clusteredLighting.frameBuffers(frameIndex);
    if (fogActive) {
        const VolumetricFogFrame fogFrame{
            .grid = froxelGrid,
            .inverseView = glm::inverse(viewMatrix),
            .previousViewProjection = cameraData.previousViewProjection,
            .historyValid = !viewFrame.cameraCut,
            .cameraPosition = cameraPosition,
            .lightDirection = frame.lightDirection,
            .lightRadiance = glm::vec3(cameraData.lightColor) * cameraData.lightDirection.w,
            .shadowCascades = frame.shadowsEnabled ? frame.cascadeCount : 0u,
            .lightViewProjections = cameraData.lightViewProjections,
            .cascadeSplits = cameraData.cascadeSplits,
            .ambientRadiance = (glm::vec3(cameraData.environmentSky) + glm::vec3(cameraData.environmentGround)) * 0.5f *
                               cameraData.environmentSky.w,
            .environmentIrradiance = frame.imageBasedLighting,
            .ambientScale = cameraData.environmentSky.w,
            .irradianceVolumeMinimum = cameraData.irradianceVolumeMinimum,
            .irradianceVolumeCellSize = cameraData.irradianceVolumeCellSize,
            .irradianceVolumeCounts = cameraData.irradianceVolumeCounts,
            .medium = *frame.fogMedium,
            .localLightCount = localLightCount,
            .shadowMap = m_shadowTexture.view,
            .shadowSampler = m_shadowSampler,
            .localLights = clusterBuffers.lights,
            .clusterRanges = clusterBuffers.clusterRanges,
            .clusterLightIndices = clusterBuffers.lightIndices,
            .environmentIrradianceBuffer = frame.environmentState.primaryIrradiance,
            .irradianceVolumeProbes = m_irradianceVolume.buffer()};
        recordVolumetricFog(draw, &fogFrame);
    } else {
        recordVolumetricFog(draw, nullptr);
    }
    if (leading) {
        // Particles simulate, cull, and sort once per frame, for this view,
        // lit by its light clusters; every view draws the result.
        const ParticleSimulationFrame particleFrame{
            .view = viewMatrix,
            .projection = projection,
            .cameraPosition = cameraPosition,
            .clusters = clusterGrid,
            .localLightCount = localLightCount,
            // The camera constants' light: the studio key outside Rendered.
            .lightDirection = glm::vec3(cameraData.lightDirection),
            .lightRadiance = glm::vec3(cameraData.lightColor) * cameraData.lightDirection.w,
            .shadowCascades = frame.shadowsEnabled ? frame.cascadeCount : 0u,
            .lightViewProjections = cameraData.lightViewProjections,
            .cascadeSplits = cameraData.cascadeSplits,
            .ambientRadiance = (glm::vec3(cameraData.environmentSky) + glm::vec3(cameraData.environmentGround)) * 0.5f *
                               cameraData.environmentSky.w,
            .environmentIrradiance = frame.imageBasedLighting,
            .ambientScale = cameraData.environmentSky.w,
            .irradianceVolumeMinimum = cameraData.irradianceVolumeMinimum,
            .irradianceVolumeCellSize = cameraData.irradianceVolumeCellSize,
            .irradianceVolumeCounts = cameraData.irradianceVolumeCounts,
            .shadowMap = m_shadowTexture.view,
            .shadowSampler = m_shadowSampler,
            .localLights = clusterBuffers.lights,
            .clusterRanges = clusterBuffers.clusterRanges,
            .clusterLightIndices = clusterBuffers.lightIndices,
            .environmentIrradianceBuffer = frame.environmentState.primaryIrradiance,
            .irradianceVolumeProbes = m_irradianceVolume.buffer()};
        if (!recordParticleSimulation(draw, scene, assets, rendering.settings().particles, particleFrame)) {
            return false;
        }
    }
    const VkImage depthImage = request.depthImage;
    const VkImageView depthView = request.depthView;
    const auto opaqueCpuBegin = std::chrono::steady_clock::now();
    if (depthPrepass) {
        passTimestamps.write(FrameTimestamp::depthPrepassBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        if (!recordDepthPrepass(draw, gpuDrivenActive, depthImage, depthView, cameraData.viewProjection,
                                viewFrame.lodView)) {
            return false;
        }
    }
    const auto prepassCpuEnd = std::chrono::steady_clock::now();
    view.depthPrepassFrame = depthPrepass ? std::optional<uint32_t>(frameIndex) : std::nullopt;
    recordAmbientOcclusion(draw, ambientOcclusion, projection, settings.antialiasing == AntialiasingMode::Taa);
    view.postProcess.prepareScene(commandBuffer, frameIndex);
    // Shading pipelines statically reference the transmission background.
    view.transmissionBackground.prepareFirstUse(commandBuffer);
    // ...and the shadow map, which a session that starts without shadows
    // (shadows Off, or a view shading mode other than Rendered) never drew.
    prepareShadowMapForSampling(commandBuffer);
    const VkImageView sceneView = view.postProcess.sceneView(frameIndex);
    const VkImageView motionView = view.postProcess.motionView(frameIndex);
    beginRendering(commandBuffer, sceneView, motionView, depthImage, depthView, renderExtent,
                   depthPrepass ? SceneAttachments::ClearColorKeepDepth : SceneAttachments::Clear,
                   view.depthInitialized);
    view.depthInitialized = true;
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
            view.depthPyramid.build(commandBuffer, renderExtent);
            passTimestamps.write(FrameTimestamp::depthPyramidEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
            m_gpuDrivenPass.cull(commandBuffer, frameIndex, GpuCullPhase::Late, cameraData.viewProjection,
                                 view.depthPyramid, viewFrame.lodView);
            restoreDepthAfterPyramid(commandBuffer, depthImage);
            beginRendering(commandBuffer, sceneView, motionView, depthImage, depthView, renderExtent,
                           SceneAttachments::Preserve, view.depthInitialized);
            setSceneViewport(draw);
            recordGpuDrivenGroups(draw, GpuCullPhase::Late, shadingOpaque);
        }
        stats.cpuOpaqueMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - opaqueCpuBegin).count();
        if (timestampQueries) {
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 2);
        }
        stats.visibleCount = m_gpuDrivenPass.retiredVisibleCount();
        if (frame.shadowCulling == ShadowCulling::Gpu) {
            // Like visibleCount, a retired frame's count: casters drawn into
            // the cascades that frame redrew.
            frameStats.shadowCasterCount = m_gpuDrivenPass.retiredFrustumViewVisibleCount();
        }
        stats.indirectCommandCount = static_cast<uint32_t>(m_gpuDrivenBatches.size());
        stats.gpuVisibilityMismatches = m_gpuDrivenPass.visibilityMismatchCount();
    } else {
        stats.indirectCommandCount = static_cast<uint32_t>(view.visibility.commands.size());
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
    if (frame.imageBasedLighting && settings.environmentBackground) {
        passTimestamps.write(FrameTimestamp::skyboxBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        m_environmentPass.recordSkybox(commandBuffer, frameIndex, draw.cameraSet, frame.environment.specularIntensity,
                                       cameraData.temporalJitter);
        passTimestamps.write(FrameTimestamp::skyboxEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    // Reflections of opaque surfaces and the background join the scene before
    // blended surfaces and outlines composite over it, and before the temporal
    // resolve accumulates it.
    if (!recordScreenSpaceReflections(draw, depthImage, depthView,
                                      plan.screenSpaceReflections ? settings.reflections
                                                                  : ReflectionQuality::ProbesOnly,
                                      {.projection = projection,
                                       .frameNumber = static_cast<uint32_t>(m_frameSerial),
                                       .temporalAccumulation = settings.antialiasing == AntialiasingMode::Taa})) {
        return false;
    }
    stats.reflectionBytes = view.postProcess.reflectionBytes();
    recordTransmissionBackground(draw, depthImage, depthView);
    recordShadedItems(draw, ShadedItems::BlendedOnly);
    // View shading: Wireframe edges go over every surface, under particles
    // and outlines.
    recordWireframeOverlay(draw, gpuDrivenActive);
    recordParticles(draw, depthImage, depthView);
    recordOutlines(draw);

    vkCmdEndRendering(commandBuffer);
    if (gpuDrivenActive) {
        m_gpuDrivenPass.finishFrame(commandBuffer, frameIndex);
    }
    if (timestampQueries) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 3);
    }
    const auto bloomCpuBegin = std::chrono::steady_clock::now();
    const TemporalResolveFrame temporalResolve{.currentToPreviousClip =
                                                   view.previousUnjitteredViewProjection *
                                                   glm::inverse(viewFrame.unjitteredViewProjection),
                                               .jitterPixels = temporal.jitterPixels,
                                               .historyValid = temporal.historyValid};
    if (!view.postProcess.record(
            commandBuffer, frameIndex, request.colorImage, request.colorView, depthImage, extent, renderExtent,
            settings, glm::inverse(viewFrame.unjitteredViewProjection), view.previousViewProjection, temporalResolve,
            depth_of_field::cameraFromProjection(projection), cameraPosition, request.grid,
            {.serial = m_frameSerial, .assetRoot = rendering.assetRoot(), .timestamps = passTimestamps},
            timestampQueries, stats.cpuBloomMilliseconds, request.colorOldLayout)) {
        return false;
    }
    stats.depthOfFieldBytes = view.postProcess.depthOfFieldBytes();
    stats.motionBlurBytes = view.postProcess.motionBlurBytes();
    view.previousViewProjection = cameraData.viewProjection;
    view.previousUnjitteredViewProjection = viewFrame.unjitteredViewProjection;
    view.previousJitterNdc = temporal.jitterNdc;
    const auto viewCpuEnd = std::chrono::steady_clock::now();
    addViewCounters(frameStats, stats);
    if (leading) {
        frameStats.cpuOpaqueMilliseconds = stats.cpuOpaqueMilliseconds;
        frameStats.cpuBlendedMilliseconds = stats.cpuBlendedMilliseconds;
        frameStats.cpuBloomMilliseconds = stats.cpuBloomMilliseconds;
        frameStats.cpuPostMilliseconds =
            std::chrono::duration<double, std::milli>(viewCpuEnd - bloomCpuBegin).count() - stats.cpuBloomMilliseconds;
        frameStats.particleEmitterCount = stats.particleEmitterCount;
        frameStats.particleCapacity = stats.particleCapacity;
    }
    if (request.stats) {
        *request.stats = {.rendered = true,
                          .width = extent.width,
                          .height = extent.height,
                          .internalWidth = renderExtent.width,
                          .internalHeight = renderExtent.height,
                          .visibleCount = stats.visibleCount,
                          .culledCount = stats.culledCount,
                          .drawCalls = stats.drawCalls,
                          .triangleCount = stats.triangleCount,
                          .temporalHistoryValid = temporal.historyValid,
                          .cpuMilliseconds =
                              std::chrono::duration<double, std::milli>(viewCpuEnd - viewCpuBegin).count()};
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

bool MeshPass::prepareGpuDrivenFrame(uint32_t frameIndex, const MeshLodView& lodView, uint32_t visibleInstanceCount) {
    m_gpuDrivenCandidates.clear();
    m_gpuDrivenKeys.clear();
    m_gpuDrivenKeySources.clear();
    // Without LOD selection the cull shader never leaves level 0, so skip the
    // coarser batches and their reserved instance slots.
    const bool lodEnabled = lodView.errorScale > 0.0f;
    for (const RenderItem& item : m_renderItems) {
        if (item.material->translucent()) {
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
        m_log.error("GPU-driven LOD instances exceed the renderer capacity");
        return false;
    }
    buildIndirectDrawPlan(m_gpuDrivenKeys, m_drawPlan);
    if (m_drawPlan.groups.size() > maximumGpuDrawGroups) {
        m_log.error("GPU-driven draw groups exceed the renderer capacity");
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
    return m_gpuDrivenPass.prepare(frameIndex, m_gpuDrivenObjects, m_gpuDrivenBatches, visibleInstanceCount);
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
    draw.viewState->postProcess.prepareNormals(commandBuffer, draw.frameIndex);
    // Only screen-space reflections read specular reflectance, but the
    // depth-normal pipelines always declare the attachment: a null view with a
    // non-UNDEFINED pipeline format needs dynamicRenderingUnusedAttachments,
    // so the target is written whenever the prepass runs.
    if (!draw.viewState->postProcess.prepareSpecular(commandBuffer, draw.frameIndex)) {
        return false;
    }
    const VkImageView normalView = draw.viewState->postProcess.normalView(draw.frameIndex);
    const VkImageView specularView = draw.viewState->postProcess.specularView(draw.frameIndex);
    beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                      SceneAttachments::Clear, draw.viewState->depthInitialized);
    setSceneViewport(draw);
    if (gpuDrivenActive) {
        // Two-phase occlusion culling runs here, so the shading pass reuses
        // both phases' compacted draws.
        recordGpuDrivenGroups(draw, GpuCullPhase::Early, depthNormal);
        vkCmdEndRendering(commandBuffer);
        prepareDepthForPyramid(commandBuffer, depthImage);
        draw.viewState->depthPyramid.build(commandBuffer, draw.renderExtent);
        m_gpuDrivenPass.cull(commandBuffer, draw.frameIndex, GpuCullPhase::Late, viewProjection,
                             draw.viewState->depthPyramid, lodView);
        restoreDepthAfterPyramid(commandBuffer, depthImage);
        beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                          SceneAttachments::Preserve, false);
        setSceneViewport(draw);
        recordGpuDrivenGroups(draw, GpuCullPhase::Late, depthNormal);
    } else {
        recordDepthNormalItems(draw);
    }
    vkCmdEndRendering(commandBuffer);
    draw.viewState->postProcess.finishNormals(commandBuffer, draw.frameIndex);
    draw.passTimestamps->write(FrameTimestamp::depthPrepassEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    // Screen-space consumers get the pyramid of the complete opaque depth.
    // With GPU-driven culling the occlusion build above already covers it
    // unless the late phase drew something, so the GPU skips this rebuild
    // when the late phase emitted nothing (the steady state): the pyramid is
    // then built once per frame. Late-drawn objects must reach the consumers,
    // so the rebuild cannot be dropped outright, and the late cull cannot use
    // last frame's pyramid instead, since objects disoccluded by camera motion
    // would be culled for a frame.
    draw.passTimestamps->write(FrameTimestamp::depthPyramidBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    prepareDepthForPyramid(commandBuffer, depthImage);
    draw.viewState->depthPyramid.build(commandBuffer, draw.renderExtent,
                                       gpuDrivenActive ? std::optional(m_gpuDrivenPass.lateEmittedCount())
                                                       : std::nullopt);
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
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size();) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (item.material->translucent()) {
            break;
        }
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < draw.viewState->visibleItems.size()) {
            const RenderItem& candidate = *draw.viewState->visibleItems[batchEnd];
            // The shading pass must test EQUAL against the same LOD's depth.
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount || candidate.material->translucent() ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        const GraphicsPipeline& pipeline =
            item.material->doubleSided ? m_depthNormalDoubleSidedPipeline : m_depthNormalPipeline;
        if (&pipeline != boundPipeline) {
            bindOpaquePipeline(draw, pipeline, depthNormal, draw.cameraSet);
            boundPipeline = &pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, static_cast<uint32_t>(batchEnd - itemIndex),
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
        itemIndex = batchEnd;
    }
}

bool MeshPass::recordScreenSpaceReflections(const DrawContext& draw, VkImage depthImage, VkImageView depthView,
                                            ReflectionQuality reflections, const ScreenSpaceReflectionCamera& camera) {
    const SceneGeometryViews geometry = sceneGeometry(*draw.viewState, draw.frameIndex);
    const bool traces = PostProcessPass::tracesReflections(geometry, reflections);
    if (traces) {
        vkCmdEndRendering(draw.commandBuffer);
    }
    if (!draw.viewState->postProcess.recordReflections(draw.commandBuffer, draw.frameIndex, geometry, reflections,
                                                       camera, draw.timestampQueries)) {
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
        beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                       draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView,
                       draw.renderExtent, SceneAttachments::Preserve, false);
        setSceneViewport(draw);
    }
    return true;
}

void MeshPass::recordTransmissionBackground(const DrawContext& draw, VkImage depthImage, VkImageView depthView) {
    const bool transmissive = std::ranges::any_of(draw.viewState->visibleItems, [](const RenderItem* item) {
        return item->material->transmission.factor > 0.0f;
    });
    if (!transmissive) {
        return;
    }
    draw.passTimestamps->write(FrameTimestamp::transmissionBackgroundBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    vkCmdEndRendering(draw.commandBuffer);
    draw.viewState->transmissionBackground.capture(
        draw.commandBuffer, draw.viewState->postProcess.sceneImage(draw.frameIndex), draw.renderExtent);
    // The resumed pass loads the motion and depth attachments stored above;
    // capture() ordered the HDR target itself.
    const VkMemoryBarrier2 attachments{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &attachments};
    vkCmdPipelineBarrier2(draw.commandBuffer, &dependency);
    beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                   draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView, draw.renderExtent,
                   SceneAttachments::Preserve, false);
    setSceneViewport(draw);
    draw.passTimestamps->write(FrameTimestamp::transmissionBackgroundEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

SceneGeometryViews MeshPass::sceneGeometry(const ViewState& view, uint32_t frameIndex) const {
    if (view.depthPrepassFrame != frameIndex) {
        return {};
    }
    return {.normals = view.postProcess.normalView(frameIndex),
            .specular = view.postProcess.specularView(frameIndex),
            .depthPyramid = view.depthPyramid.view(),
            .depthPyramidSampler = view.depthPyramid.sampler(),
            .depthPyramidLevels = view.depthPyramid.levelCount(),
            .renderExtent = view.depthPyramid.renderExtent(),
            .storageExtent = view.depthPyramid.storageExtent()};
}

void MeshPass::recordVolumetricFog(const DrawContext& draw, const VolumetricFogFrame* frame) {
    if (frame == nullptr) {
        // Keeps the bound volume free of fog; records nothing once cleared.
        draw.viewState->volumetricFog.recordCleared(draw.commandBuffer);
        return;
    }
    prepareShadowMapForSampling(draw.commandBuffer);
    draw.passTimestamps->write(FrameTimestamp::volumetricFogBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    draw.viewState->volumetricFog.record(draw.commandBuffer, draw.frameIndex, *frame);
    draw.passTimestamps->write(FrameTimestamp::volumetricFogEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

void MeshPass::prepareShadowMapForSampling(VkCommandBuffer commandBuffer) {
    if (m_shadowInitialized) {
        return;
    }
    // Fog and particles bind the shadow map even when no cascade was ever
    // rendered; they sample it only for rendered cascades.
    imageBarrier(commandBuffer, m_shadowTexture.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, maximumShadowCascades);
    m_shadowInitialized = true;
}

bool MeshPass::recordParticleSimulation(const DrawContext& draw, const Scene& scene, const AssetManager& assets,
                                        ParticleQuality quality, const ParticleSimulationFrame& frame) {
    const std::vector<particles::EmitterSnapshot> emitters =
        quality == ParticleQuality::Off ? std::vector<particles::EmitterSnapshot>{} : particles::gatherEmitters(scene);
    // Sprite slot 0 is the procedural sprite; its view only keeps the
    // descriptor array valid. Emitters share a slot per distinct texture.
    m_particleSpriteViews.fill(m_whiteTexture.view);
    uint32_t usedSlots = 1;
    std::vector<uint32_t> textureSlots(emitters.size(), 0u);
    for (size_t index = 0; index < emitters.size(); ++index) {
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        const TextureHandle texture = emitters[index].emitter.texture;
        if (!texture || !uiTexture(texture, assets, sampler, view)) {
            continue;
        }
        const auto used = m_particleSpriteViews.begin() + usedSlots;
        const auto found = std::find(m_particleSpriteViews.begin() + 1, used, view);
        if (found != used) {
            textureSlots[index] = static_cast<uint32_t>(found - m_particleSpriteViews.begin());
        } else if (usedSlots < particles::maximumSpriteTextures) {
            m_particleSpriteViews[usedSlots] = view;
            textureSlots[index] = usedSlots++;
        }
    }
    if (!emitters.empty()) {
        prepareShadowMapForSampling(draw.commandBuffer);
    }
    draw.passTimestamps->write(FrameTimestamp::particleSimulationBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const bool recorded = m_particles.recordSimulation(
        draw.commandBuffer, draw.frameIndex, m_frameSerial, quality,
        {.emitters = emitters, .textureSlots = textureSlots, .sceneGeneration = scene.contentGeneration()}, frame);
    draw.passTimestamps->write(FrameTimestamp::particleSimulationEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    draw.stats->particleEmitterCount = m_particles.emitterCount();
    draw.stats->particleCapacity = m_particles.capacity();
    return recorded;
}

void MeshPass::recordParticles(const DrawContext& draw, VkImage depthImage, VkImageView depthView) {
    if (!m_particles.drawPending()) {
        return;
    }
    vkCmdEndRendering(draw.commandBuffer);
    draw.passTimestamps->write(FrameTimestamp::particleDrawBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    m_particles.recordDraw(draw.commandBuffer, {.sceneColor = draw.viewState->postProcess.sceneView(draw.frameIndex),
                                                .motion = draw.viewState->postProcess.motionView(draw.frameIndex),
                                                .depthImage = depthImage,
                                                .depthView = depthView,
                                                .renderExtent = draw.renderExtent,
                                                .cameraSet = draw.cameraSet,
                                                .view = draw.view,
                                                .spriteViews = m_particleSpriteViews,
                                                .spriteSampler = m_sampler});
    draw.passTimestamps->write(FrameTimestamp::particleDrawEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                   draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView, draw.renderExtent,
                   SceneAttachments::Preserve, false);
    setSceneViewport(draw);
}

void MeshPass::recordAmbientOcclusion(const DrawContext& draw, AmbientOcclusionQuality quality,
                                      const glm::mat4& projection, bool temporalNoise) {
    if (draw.timestampQueries) {
        vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, draw.timestampQueries, 10);
    }
    const SceneGeometryViews geometry = sceneGeometry(*draw.viewState, draw.frameIndex);
    if (quality == AmbientOcclusionQuality::Off || !geometry.valid()) {
        draw.viewState->ambientOcclusion.recordUnoccluded(draw.commandBuffer, draw.frameIndex);
    } else {
        draw.viewState->ambientOcclusion.record(
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

bool MeshPass::collectCpuSubmittedItems(ViewState& view, bool gpuDrivenActive, const glm::vec3& cameraPosition,
                                        std::span<const RenderObjectId> outlinedObjects) {
    // RenderItem::outlined marks every recorded view's outlines; this view
    // keeps only its own.
    const auto outlinedHere = [&](const RenderItem& item) {
        return item.outlined &&
               std::ranges::binary_search(outlinedObjects, static_cast<RenderObjectId>(entt::to_integral(item.entity)));
    };
    view.visibleItems.clear();
    for (GpuSceneHandle handle : view.visibility.visibleInstances) {
        const auto found = m_gpuHandleItems.find(handleKey(handle));
        if (found == m_gpuHandleItems.end()) {
            continue;
        }
        const RenderItem& item = m_renderItems[found->second];
        // With GPU culling the CPU only submits what the indirect path cannot:
        // blended surfaces and the editor's outline passes.
        if (gpuDrivenActive && !item.material->translucent() && !outlinedHere(item)) {
            continue;
        }
        view.visibleItems.push_back(&item);
    }
    std::stable_sort(view.visibleItems.begin(), view.visibleItems.end(),
                     [&cameraPosition](const RenderItem* first, const RenderItem* second) {
                         const bool firstBlended = first->material->translucent();
                         const bool secondBlended = second->material->translucent();
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
    if (view.visibleItems.size() > maximumObjectsPerFrame) {
        m_log.error("Visible object count exceeds the per-frame render capacity");
        return false;
    }
    view.visibleOutlined.clear();
    for (const RenderItem* item : view.visibleItems) {
        view.visibleOutlined.push_back(outlinedHere(*item) ? 1u : 0u);
    }
    return true;
}

void MeshPass::uploadCpuSubmittedObjects(const DrawContext& draw) {
    const ViewState& view = *draw.viewState;
    void* mapped =
        view.sharesFrameSets ? m_cameras[draw.frameIndex].objectsMapped : view.cameras[draw.frameIndex].objectsMapped;
    const VmaAllocation allocation = view.sharesFrameSets ? m_cameras[draw.frameIndex].objects.allocation
                                                          : view.cameras[draw.frameIndex].objects.allocation;
    auto* objectData = static_cast<ObjectData*>(mapped) + draw.objectBase;
    for (size_t index = 0; index < view.visibleItems.size(); ++index) {
        const RenderItem& item = *view.visibleItems[index];
        objectData[index].model = item.world;
        objectData[index].previousModel = item.previousWorld;
        objectData[index].skin = {item.skinPaletteOffset, static_cast<uint32_t>(item.jointPalette.size()),
                                  item.gpuMaterial->index, 0};
    }
    vmaFlushAllocation(m_allocator, allocation, sizeof(ObjectData) * draw.objectBase,
                       sizeof(ObjectData) * std::max<size_t>(view.visibleItems.size(), 1));
}

bool MeshPass::recordPicks(const DrawContext& draw, const Scene& scene, const MeshPassViewRequest& request,
                           const ViewFrame& viewFrame) {
    // Pick regions address output pixels, so the id buffer uses the view's
    // camera unjittered at the output aspect, whatever the internal resolution.
    glm::mat4 projection = glm::perspectiveRH_ZO(
        viewFrame.verticalFov, static_cast<float>(request.extent.width) / static_cast<float>(request.extent.height),
        viewFrame.farPlane, viewFrame.nearPlane);
    projection[1][1] *= -1.0f;
    const glm::mat4 viewProjection = projection * viewFrame.view;
    // Pickable: the view's frustum-visible items, the list both CPU and
    // GPU-driven submission draw from (the latter culls further by occlusion,
    // which the id buffer's depth test replaces). Each entity gets one id, so
    // its primitives share it; id 0 means "no object".
    std::vector<const RenderItem*> items;
    std::vector<uint32_t> itemIds;
    std::vector<PickedObject> objects;
    std::unordered_map<Entity, uint32_t> entityIds;
    for (GpuSceneHandle handle : draw.viewState->visibility.visibleInstances) {
        const auto found = m_gpuHandleItems.find(handleKey(handle));
        if (found == m_gpuHandleItems.end()) {
            continue;
        }
        const RenderItem& item = m_renderItems[found->second];
        const auto [entry, inserted] = entityIds.try_emplace(item.entity, static_cast<uint32_t>(objects.size() + 1));
        if (inserted) {
            objects.push_back({.entity = scene.id(item.entity),
                               .object = static_cast<RenderObjectId>(entt::to_integral(item.entity))});
        }
        items.push_back(&item);
        itemIds.push_back(entry->second);
    }
    for (MeshPassPick& pick : request.picks) {
        if (pick.readback == nullptr) {
            m_log.error("A pick region has no readback");
            return false;
        }
        // A region whose targets cannot be allocated fails alone; the frame
        // still renders.
        if (!m_idBuffer.begin(draw.commandBuffer, m_frameSerial, request.extent, pick.region, draw.cameraSet)) {
            m_log.error("Cannot allocate the id buffer for a " + std::to_string(pick.region.width) + "x" +
                        std::to_string(pick.region.height) + " pick region");
            continue;
        }
        std::optional<uint32_t> boundPage;
        for (size_t index = 0; index < items.size(); ++index) {
            const RenderItem& item = *items[index];
            bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
            m_idBuffer.draw(draw.commandBuffer,
                            {.modelViewProjection = viewProjection * item.world,
                             .firstJoint = item.skinPaletteOffset,
                             .jointCount = static_cast<uint32_t>(item.jointPalette.size()),
                             .objectId = itemIds[index]},
                            item.lod.indexCount, item.gpuMesh->geometry.firstIndex + item.lod.firstIndex,
                            item.gpuMesh->geometry.vertexOffset);
        }
        m_idBuffer.end(draw.commandBuffer, *pick.readback);
        pick.recorded = true;
        pick.objects = objects;
        pick.viewProjection = viewProjection;
        ++draw.stats->pickPasses;
    }
    return true;
}

void MeshPass::recordSelectionMask(const DrawContext& draw) {
    if (std::ranges::none_of(draw.viewState->visibleOutlined, [](uint8_t outlined) { return outlined != 0; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.layout(), 0, 1,
                            &draw.cameraSet, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (draw.viewState->visibleOutlined[itemIndex] == 0) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordShadedItems(const DrawContext& draw, ShadedItems selection) {
    const VkDescriptorSet sets[]{draw.cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    const bool blendedOnly = selection == ShadedItems::BlendedOnly;
    const bool opaqueOnly = selection == ShadedItems::OpaqueOnly;
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    const auto opaqueCpuBegin = std::chrono::steady_clock::now();
    auto blendedCpuBegin = opaqueCpuBegin;
    // With GPU culling the opaque timestamp was already written after the
    // indirect draws.
    bool opaqueFinished = blendedOnly;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size();) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        const bool blended = item.material->translucent();
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
        while (batchEnd < draw.viewState->visibleItems.size()) {
            const RenderItem& candidate = *draw.viewState->visibleItems[batchEnd];
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->alphaMode != item.material->alphaMode ||
                candidate.material->translucent() != item.material->translucent() ||
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
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
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
    if (std::ranges::none_of(draw.viewState->visibleOutlined, [](uint8_t outlined) { return outlined != 0; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.layout(), 0, 1,
                            &draw.cameraSet, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (draw.viewState->visibleOutlined[itemIndex] == 0) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        const glm::vec4 outlineSettings{item.mesh->bounds.center, 2.0f};
        vkCmdPushConstants(draw.commandBuffer, m_outlinePipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(outlineSettings), &outlineSettings);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordWireframeOverlay(const DrawContext& draw, bool gpuDrivenActive) {
    if (!draw.pipelines->hasWireframeOverlay()) {
        return;
    }
    const OpaquePipelines overlay{.singleSided = &draw.pipelines->wireframe,
                                  .doubleSided = &draw.pipelines->wireframeDoubleSided};
    if (gpuDrivenActive) {
        // Both cull phases' compacted draws are complete inside the scene pass.
        recordGpuDrivenGroups(draw, GpuCullPhase::Early, overlay);
        recordGpuDrivenGroups(draw, GpuCullPhase::Late, overlay);
    }
    // CPU-submitted items: every surface without GPU culling, else the
    // blended ones (outlined opaque items are also listed then, but their
    // edges came from the indirect groups). Unlike recordShadedItems this adds
    // no visibility or timing statistics; the edges belong to surfaces already
    // counted.
    const VkDescriptorSet sets[]{draw.cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (gpuDrivenActive && !item.material->translucent()) {
            continue;
        }
        const GraphicsPipeline* pipeline = item.material->doubleSided ? overlay.doubleSided : overlay.singleSided;
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
            vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0,
                                    static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
            boundPipeline = pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordDeferredUploads(const RenderSettings& settings, RenderStats& stats) {
    m_deferredUploads.setConfiguration({.warningFrames = settings.deferredUploadWarningFrames});
    const DeferredUploadFrameSummary deferred = m_deferredUploads.endFrame();
    stats.deferredMeshUploads = deferred.deferredMeshes;
    stats.deferredTextureUploads = deferred.deferredTextures;
    for (const DeferredUploadWarning& warning : deferred.overdue) {
        m_log.warning(describeDeferredUpload(warning));
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
    // UI descriptor sets keep the view they were given, so a UI image gets
    // every level now and its views never change afterwards.
    if (!texture || !completeTextureMips(handleKey(handle), assets)) {
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
    for (std::unique_ptr<ViewState>& view : m_views) {
        if (view) {
            destroyViewState(context, *view);
        }
    }
    m_views.clear();
    m_uploadContext.shutdown();
    m_textureUploadContext.shutdown();
    for (auto& [variant, pipelines] : m_meshPipelines) {
        pipelines->shutdown(context.device());
    }
    m_meshPipelines.clear();
    m_shadowPipeline.shutdown(context.device());
    m_selectionMaskPipeline.shutdown(context.device());
    m_outlinePipeline.shutdown(context.device());
    m_environmentPass.shutdown();
    m_irradianceVolume.shutdown();
    m_depthNormalPipeline.shutdown(context.device());
    m_depthNormalDoubleSidedPipeline.shutdown(context.device());
    m_particles.shutdown();
    m_idBuffer.shutdown();
    m_particleSpriteViews.fill(VK_NULL_HANDLE);
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
}
