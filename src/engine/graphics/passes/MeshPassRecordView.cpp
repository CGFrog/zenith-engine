// MeshPass: recording one render view.

#include "../Decals.hpp"
#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"
#include "../RenderViewPlanning.hpp"
#include "../ViewShading.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

using namespace mesh_pass_detail;

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
    // A view's own shadow maps (zero for views sampling the frame's).
    frame.shadowCacheBytes += view.shadowCacheBytes;
    frame.localShadowAtlasBytes += view.localShadowAtlasBytes;
}

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
} // namespace

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
    if (!leading) {
        // The frame stage selected the leading view's LODs.
        selectRenderItemLods(viewFrame.lodView);
    }
    // A view isolated differently from the frame's shadow casters draws its
    // own shadow maps, with tiles for its own lights.
    std::vector<int32_t> ownShadowTiles;
    const std::span<const LocalLight> shadowedLights(
        lights.data(), std::min<size_t>(lights.size(), view.clusteredLighting.lightCapacity()));
    // A secondary scene's preview lighting casts no shadows.
    if (!frame.previewScene &&
        !updateViewShadows(commandBuffer, frameIndex, view, request, frame, shadowedLights, cameraPosition,
                           viewFrame.lodView, scene, assets, ownShadowTiles, stats)) {
        return false;
    }
    ClusterLightStatistics clusterStatistics;
    const uint32_t localLightCount = view.clusteredLighting.beginFrame(frameIndex, lights, clusterStatistics);
    // Only the leading view's lights had the frame's shadow tiles rendered;
    // another view finds a light's tiles by its stable id.
    if (view.shadows) {
        view.clusteredLighting.assignShadowTiles(frameIndex, ownShadowTiles);
    } else {
        view.clusteredLighting.assignShadowTiles(
            frameIndex,
            leading ? frame.leadShadowTiles : remapLocalShadowTiles(frame.leadLights, frame.leadShadowTiles, lights));
    }
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
    if (!collectCpuSubmittedItems(view, gpuDrivenActive, cameraPosition, request.outlinedObjects,
                                  request.visibleObjects)) {
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
            .shadowMap = viewShadowMap(view),
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
            .shadowMap = viewShadowMap(view),
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
    prepareShadowMapForSampling(commandBuffer, m_shadowMap);
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
    const bool shadesSurfaces = drawsSurfaces(settings.viewShading);
    if (!gpuDrivenActive) {
        if (shadesSurfaces) {
            recordShadedItems(draw, ShadedItems::OpaqueOnly);
        } else if (timestampQueries) {
            // Wireframe shades nothing: close the opaque span the shading
            // would have closed, so the frame's timestamps stay complete.
            vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueries, 2);
        }
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
    if (shadesSurfaces) {
        recordShadedItems(draw, ShadedItems::BlendedOnly);
    }
    // View shading: Wireframe draws every edge of every drawn surface on the
    // cleared scene, under particles and outlines.
    recordWireframeOverlay(draw, gpuDrivenActive);
    // Particles are the frame scene's.
    if (!frame.previewScene) {
        recordParticles(draw, depthImage, depthView);
    }
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
    view.recordedCamera = MeshPassViewCamera{.viewProjection = viewFrame.unjitteredViewProjection,
                                             .nearPlane = viewFrame.nearPlane,
                                             .farPlane = viewFrame.farPlane,
                                             .renderExtent = renderExtent};
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
        frameStats.droppedParticleEmitters = stats.droppedParticleEmitters;
    }
    if (request.stats) {
        // The work on the maps the view sampled: its own, or the frame's.
        const RenderStats& shadowStats = view.shadows ? stats : frameStats;
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
                              std::chrono::duration<double, std::milli>(viewCpuEnd - viewCpuBegin).count(),
                          .ownShadowMaps = view.shadows != nullptr,
                          .shadowCascadesRendered = shadowStats.shadowCascadesRendered,
                          .shadowCasterCount = shadowStats.shadowCasterCount,
                          .localShadowTilesRendered = shadowStats.localShadowTilesRendered,
                          .localShadowCasterCount = shadowStats.localShadowCasterCount};
    }
    return true;
}
