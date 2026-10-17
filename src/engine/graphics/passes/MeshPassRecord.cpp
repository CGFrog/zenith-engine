// MeshPass::record: per-view frame preparation and the frame's recording stages.

#include "../IrradianceVolume.hpp"
#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"
#include "../SpatialUpscale.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

using namespace mesh_pass_detail;

namespace {
glm::vec3 normalizedLightDirection(const glm::vec3& direction) {
    constexpr float minimumDirectionLength = 0.0001f;
    if (glm::dot(direction, direction) < minimumDirectionLength * minimumDirectionLength) {
        return glm::normalize(DirectionalLightComponent{}.direction);
    }
    return glm::normalize(direction);
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
                                                        const glm::vec3& cameraPosition, SceneVisibilityMode visibility,
                                                        std::vector<EnvironmentSource>& sources) {
    std::vector<ReflectionProbeVolume> mapped;
    std::vector<EnvironmentSource> mappedSources;
    for (const ReflectionProbeVolume& probe : gatherReflectionProbes(scene, visibility)) {
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
} // namespace

void MeshPass::advanceSurfaceAnimation() {
    // Long hitches advance the waves at most this far, like particles.
    constexpr double maximumStepSeconds = 0.1;
    // Waves are not periodic, so the clock wraps rarely; float seconds keep
    // millisecond precision well past this.
    constexpr double wrapSeconds = 4096.0;
    double step = 0.0;
    if (m_fixedFrameSeconds) {
        step = *m_fixedFrameSeconds;
    } else {
        const auto now = std::chrono::steady_clock::now();
        if (m_previousAnimationTime) {
            step = std::chrono::duration<double>(now - *m_previousAnimationTime).count();
        }
        m_previousAnimationTime = now;
    }
    m_surfaceAnimationSeconds =
        std::fmod(m_surfaceAnimationSeconds + std::clamp(step, 0.0, maximumStepSeconds), wrapSeconds);
}

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
        if (request.scene != nullptr && (request.view == 0 || !request.sceneId)) {
            m_log.error("Render view request " + std::to_string(index) +
                        " draws a secondary scene from the default view or without its handle");
            return false;
        }
    }
    // The frame's scene's views lead; each secondary scene's views follow,
    // grouped by scene.
    const auto firstSecondary =
        std::ranges::find_if(views, [](const MeshPassViewRequest& request) { return request.scene != nullptr; });
    const std::span<const MeshPassViewRequest> frameViews(views.begin(), firstSecondary);
    std::vector<std::span<const MeshPassViewRequest>> secondaryScenes;
    for (auto group = firstSecondary; group != views.end();) {
        const auto end = std::find_if(group, views.end(), [&](const MeshPassViewRequest& request) {
            return request.scene == nullptr || !(request.sceneId == group->sceneId) || request.scene != group->scene;
        });
        if (end != views.end() && end->scene == nullptr) {
            m_log.error("Render views of secondary scenes must follow every view of the frame's scene");
            return false;
        }
        if (std::ranges::any_of(secondaryScenes, [&](std::span<const MeshPassViewRequest> earlier) {
                return earlier.front().sceneId == group->sceneId;
            })) {
            m_log.error("Render views of one secondary scene must be adjacent");
            return false;
        }
        secondaryScenes.emplace_back(group, end);
        group = end;
    }
    if (frameViews.empty()) {
        m_log.error("A frame must record at least one view of the frame's scene");
        return false;
    }

    const double previousGpuFrameMilliseconds = rendering.m_stats.gpuFrameMilliseconds;
    rendering.m_stats = {};

    // The caller waited for this frame slot's fence, so every frame at least
    // one full ring older than this one has completed on the GPU.
    ++m_frameSerial;
    advanceSurfaceAnimation();
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

    m_sceneVisibilityMode = rendering.sceneVisibilityMode();
    retireSecondaryScenes(rendering.takeDestroyedScenes());
    // Each scene's render packets mark the union of its views' outlines;
    // views filter. Secondary scenes build before the frame stage writes the
    // material records and bindless slots their packets may add.
    const auto secondaryBuildBegin = std::chrono::steady_clock::now();
    for (std::span<const MeshPassViewRequest> sceneViews : secondaryScenes) {
        PacketBuild build;
        if (!buildSecondaryScene(assets, sceneViews, build)) {
            return false;
        }
        rendering.m_stats.secondaryScenePacketRebuilds += build.rebuilt ? 1u : 0u;
        rendering.m_stats.secondaryScannedRenderables += build.scannedRenderables;
    }
    rendering.m_stats.secondaryScenesDrawn = static_cast<uint32_t>(secondaryScenes.size());
    double secondaryMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - secondaryBuildBegin).count();
    m_outlinedObjects = outlineUnion(frameViews);

    // The frame's shadow maps live in the frame's camera sets, so they cast
    // from the isolation of the view drawing through those sets (the default
    // view, else the leading one); a view isolated differently draws its own
    // (updateViewShadows).
    const auto frameSetsView = std::ranges::find(views, 0u, &MeshPassViewRequest::view);
    const MeshPassViewRequest& shadowOwner = frameSetsView != views.end() ? *frameSetsView : views.front();
    if (shadowOwner.visibleObjects == nullptr) {
        m_frameShadowCasters.reset();
    } else if (!m_frameShadowCasters || *m_frameShadowCasters != *shadowOwner.visibleObjects) {
        m_frameShadowCasters = *shadowOwner.visibleObjects;
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
    for (size_t index = 0; index < frameViews.size(); ++index) {
        const bool leading = index == 0;
        const MeshPassViewRequest& request = frameViews[index];
        const ViewFrame viewFrame =
            leading ? leadFrame
                    : prepareViewFrame(*m_views[request.view], request, scene, previousGpuFrameMilliseconds);
        if (!recordView(commandBuffer, frameIndex, scene, assets, rendering, request, viewFrame, leading, frameTasks,
                        leading ? timestampQueries : VK_NULL_HANDLE, leading ? passTimestamps : noTimestamps, frame)) {
            return false;
        }
    }
    uint32_t skinMatrixCount = frame.skinMatrixCount;
    const auto secondaryRecordBegin = std::chrono::steady_clock::now();
    for (std::span<const MeshPassViewRequest> sceneViews : secondaryScenes) {
        if (!recordSecondaryScene(commandBuffer, frameIndex, assets, rendering, sceneViews, frameTasks,
                                  previousGpuFrameMilliseconds, frame, skinMatrixCount)) {
            return false;
        }
    }
    secondaryMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - secondaryRecordBegin).count();
    rendering.m_stats.cpuSecondarySceneMilliseconds = secondaryMilliseconds;
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
    // Only what the frame's visibility mode draws lights it: `renderable` off,
    // or the editor hide in Editor mode, removes a light, environment, or fog.
    frame.light = frameDirectionalLight(scene, m_sceneVisibilityMode);
    frame.environment = frameEnvironmentLight(scene, m_sceneVisibilityMode);
    frame.fogMedium = frameVolumetricFog(scene, m_sceneVisibilityMode);
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
                               ? selectPerPixelProbes(scene, assets, environment, cameraPosition, m_sceneVisibilityMode,
                                                      residentProbeSources)
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
    const Entity irradianceVolumeEntity =
        settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume
            ? irradiance_volume::selectVolume(scene, cameraPosition, m_sceneVisibilityMode)
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

    const auto packetBuildStart = std::chrono::steady_clock::now();
    PacketBuild build;
    if (!buildScenePackets(scene, assets, build)) {
        return false;
    }
    const bool rebuildPackets = build.rebuilt;
    const SceneRevisions& revisions = scene.revisions();
    writeMaterialRecords(frameIndex, assets);
    streamTextureMips(assets, cameraPosition);
    updateBindlessDescriptor(frameIndex);
    const auto packetBuildEnd = std::chrono::steady_clock::now();
    const double packetBuildMilliseconds =
        std::chrono::duration<double, std::milli>(packetBuildEnd - packetBuildStart).count();
    const FrameResourceUse lightResources[]{{"scene.lights", FrameResourceAccess::Read},
                                            {"frame.light_list", FrameResourceAccess::Write}};
    const FrameResourceUse visibilityResources[]{{"renderer.persistent_scene", FrameResourceAccess::Read},
                                                 {"frame.visibility", FrameResourceAccess::Write}};
    FrameTaskGraph taskGraph(++m_taskGeneration);
    taskGraph.add("light-selection", lightResources, [&](const FrameTaskContext&) {
        if (frame.localLightLimit > 0) {
            frame.sceneLights = gatherLocalLights(scene, m_sceneVisibilityMode);
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
    stats.scannedRenderables = build.scannedRenderables;
    stats.updatedObjectRecords = build.updatedObjectRecords;
    stats.mainThreadAllocations = build.allocations;
    recordDeferredUploads(settings, stats);
    recordTextureStreaming(assets, stats);
    stats.taskQueueDepth = taskResult.stats.maximumQueueDepth;
    stats.taskStolenJobs = taskResult.stats.stolenJobs;
    stats.taskWorkerUtilization = taskResult.stats.workerUtilization;
    stats.taskMainThreadAssistMilliseconds = taskResult.stats.mainThreadAssistMilliseconds;
    stats.taskGraphLatencyMilliseconds = taskResult.stats.latencyMilliseconds;
    stats.renderableCount = static_cast<uint32_t>(m_renderItems.size());
    stats.droppedRenderItems = m_droppedRenderItems + m_droppedMaterialItems;
    stats.particleBytes = m_particles.allocatedBytes();
    stats.irradianceVolumeBytes = m_irradianceVolume.allocatedBytes();
    stats.irradianceVolumeProbes =
        irradianceVolume ? static_cast<uint32_t>(irradianceVolume->bakedData->probes.size()) : 0u;
    stats.shadowCacheBytes =
        static_cast<uint64_t>(shadowResolution) * shadowResolution * sizeof(float) * maximumShadowCascades;
    DirectionalLightComponent shadowLight = light;
    shadowLight.shadows.enabled = frame.shadowsEnabled;
    uint32_t droppedSkinnedItems = 0;
    {
        // The identity matrix every unskinned draw indexes.
        auto* skinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].skinPalettesMapped);
        auto* previousSkinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].previousSkinPalettesMapped);
        skinMatrices[0] = glm::mat4(1.0f);
        previousSkinMatrices[0] = glm::mat4(1.0f);
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].skinPalettes.allocation, 0, sizeof(glm::mat4));
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].previousSkinPalettes.allocation, 0, sizeof(glm::mat4));
    }
    frame.skinMatrixCount = writeSkinPalettes(frameIndex, 1, droppedSkinnedItems);
    stats.droppedSkinnedRenderItems = droppedSkinnedItems;
    if (m_skinPaletteOverflow.update(droppedSkinnedItems > 0)) {
        m_log.warning("Skin palettes exceed the per-frame capacity of " + std::to_string(maximumSkinMatricesPerFrame) +
                      " joint matrices; " + std::to_string(droppedSkinnedItems) +
                      " skinned render items draw in their bind pose");
    }
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
    const std::vector<RenderObjectId>* frameCasters = m_frameShadowCasters ? &*m_frameShadowCasters : nullptr;
    const std::array<bool, maximumShadowCascades> dirtyCascades =
        dirtyShadowCascades(m_shadowMap, revisions, frameCasters, cameraData.lightViewProjections, cascadeCount);
    // Isolated casters imply an isolated default view (CPU submission) or no
    // default view (no GPU-driven submission), so they are culled on the CPU.
    if (!recordShadow(commandBuffer, m_shadowMap, frameShadowTarget(frameIndex), frameIndex, m_renderItems, shadowLight,
                      cameraData.lightViewProjections, cascadeCount, dirtyCascades, frame.shadowCulling, frameCasters,
                      leadFrame.lodView, passTimestamps, stats)) {
        return false;
    }
    rememberShadowCascades(m_shadowMap, revisions, frameCasters, cameraData.lightViewProjections, cascadeCount);
    stats.cpuShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowCpuBegin).count();
    return true;
}
