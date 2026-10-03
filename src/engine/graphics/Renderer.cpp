#include "Renderer.hpp"
#include "ColorGrading.hpp"
#include "RenderViewPlanning.hpp"
#include "ViewShading.hpp"
#include "vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t surfaceRetryDelayMs = 16;

// ZENITH_FORCE_UPSCALER=off|taau|fsr1 and ZENITH_FORCE_UPSCALER_QUALITY=
// quality|balanced|performance; documented in docs/RENDERER_VALIDATION.md.
// Unknown values are ignored. TAAU also forces TAA, which it requires.
void applyUpscalerOverrides(RenderSettings& settings) {
    if (const char* mode = SDL_getenv("ZENITH_FORCE_UPSCALER")) {
        const std::string_view value{mode};
        if (value == "off" || value == "native") {
            settings.upscaler.mode = UpscalerMode::Off;
        } else if (value == "taau") {
            settings.upscaler.mode = UpscalerMode::Taau;
            settings.antialiasing = AntialiasingMode::Taa;
        } else if (value == "fsr1") {
            settings.upscaler.mode = UpscalerMode::Fsr1;
        }
    }
    if (const char* quality = SDL_getenv("ZENITH_FORCE_UPSCALER_QUALITY")) {
        const std::string_view value{quality};
        if (value == "quality") {
            settings.upscaler.quality = UpscalerQuality::Quality;
        } else if (value == "balanced") {
            settings.upscaler.quality = UpscalerQuality::Balanced;
        } else if (value == "performance") {
            settings.upscaler.quality = UpscalerQuality::Performance;
        }
    }
}

// ZENITH_FORCE_VIEW_SHADING=rendered|material_preview|solid|wireframe;
// documented in docs/RENDERER_VALIDATION.md. Unknown values are ignored.
void applyViewShadingOverride(RenderSettings& settings) {
    const char* mode = SDL_getenv("ZENITH_FORCE_VIEW_SHADING");
    if (mode == nullptr) {
        return;
    }
    const std::string_view value{mode};
    if (value == "rendered") {
        settings.viewShading = ViewShading::Rendered;
    } else if (value == "material_preview") {
        settings.viewShading = ViewShading::MaterialPreview;
    } else if (value == "solid") {
        settings.viewShading = ViewShading::Solid;
    } else if (value == "wireframe") {
        settings.viewShading = ViewShading::Wireframe;
    }
}

void applyDiagnosticOverrides(RenderRuntime& rendering) {
    RenderSettings settings = rendering.settings();
    if (SDL_getenv("ZENITH_FORCE_TAA") != nullptr) {
        settings.antialiasing = AntialiasingMode::Taa;
    }
    // Every renderer feature at its highest quality (ambient occlusion at the
    // half-resolution default, which matches full resolution visually);
    // documented with the other diagnostic variables in docs/RENDERER_VALIDATION.md.
    if (SDL_getenv("ZENITH_FORCE_HIGH_QUALITY") != nullptr) {
        settings.antialiasing = AntialiasingMode::Taa;
        settings.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
        settings.reflections = ReflectionQuality::ScreenSpaceFullResolution;
        settings.environment = EnvironmentQuality::ImageBasedHigh;
        settings.reflectionProbes = true;
        settings.reflectionProbeBlending = ReflectionProbeBlending::PerPixel;
        settings.localLights = LocalLightingQuality::Clustered;
        settings.localShadows = LocalShadowQuality::High;
        settings.depthPrepass = true;
        settings.shadows = ShadowQuality::CascadedHigh;
        settings.bloom = BloomQuality::Downsampled;
        settings.volumetricFog = VolumetricFogQuality::High;
        settings.indirectDiffuse = IndirectDiffuseMode::ProbeVolume;
        settings.motionBlur = MotionBlurQuality::High;
        settings.depthOfField.quality = DepthOfFieldQuality::High;
        settings.decals = DecalQuality::On;
        settings.particles = ParticleQuality::High;
    }
    if (SDL_getenv("ZENITH_DISABLE_MOTION_BLUR") != nullptr) {
        settings.motionBlur = MotionBlurQuality::Off;
    }
    if (SDL_getenv("ZENITH_DISABLE_VOLUMETRIC_FOG") != nullptr) {
        settings.volumetricFog = VolumetricFogQuality::Off;
    }
    if (SDL_getenv("ZENITH_DISABLE_DEPTH_OF_FIELD") != nullptr) {
        settings.depthOfField.quality = DepthOfFieldQuality::Off;
    }
    if (SDL_getenv("ZENITH_DISABLE_PARTICLES") != nullptr) {
        settings.particles = ParticleQuality::Off;
    }
    if (SDL_getenv("ZENITH_FORCE_DYNAMIC_RESOLUTION") != nullptr) {
        settings.dynamicResolution.enabled = true;
        settings.dynamicResolution.targetGpuMilliseconds = 0.01f;
    }
    applyUpscalerOverrides(settings);
    applyViewShadingOverride(settings);
    rendering.setSettings(settings);
}

void applyGraphBarrier(VkCommandBuffer commandBuffer, VkImage image, const RenderGraphBarrier& planned) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = planned.sourceStages,
                                  .srcAccessMask = planned.sourceAccess,
                                  .dstStageMask = planned.destinationStages,
                                  .dstAccessMask = planned.destinationAccess,
                                  .oldLayout = static_cast<VkImageLayout>(planned.oldLayout),
                                  .newLayout = static_cast<VkImageLayout>(planned.newLayout),
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = 1,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

RenderCapabilities detectRenderCapabilities(VkPhysicalDevice device) {
    VkPhysicalDeviceVulkan12Features vulkan12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vulkan12};
    vkGetPhysicalDeviceFeatures2(device, &features);
    const bool descriptorIndexing = vulkan12.runtimeDescriptorArray && vulkan12.descriptorBindingPartiallyBound &&
                                    vulkan12.descriptorBindingVariableDescriptorCount &&
                                    vulkan12.shaderSampledImageArrayNonUniformIndexing;
    return {.descriptorIndexing = descriptorIndexing,
            .multiDrawIndirect = features.features.multiDrawIndirect == VK_TRUE,
            .indirectDrawCount = vulkan12.drawIndirectCount == VK_TRUE,
            .gpuDrivenPath = false};
}

// Settings-driven choices come from the frame's and each view's settings;
// every choice MeshPass makes while recording comes from the view's plan,
// never re-derived here.
void fillRenderGraphShape(std::span<const MeshPassViewRequest> views, bool defaultViewOffscreen,
                          const RenderSettings& settings, const RenderCapabilities& capabilities,
                          RenderGraphShape& shape) {
    const MeshPassFramePlan& leadPlan = views.front().plan;
    const bool imageBasedLighting = settings.environment != EnvironmentQuality::Hemisphere;
    shape.shadows = settings.shadows != ShadowQuality::Off;
    shape.imageBasedLighting = imageBasedLighting;
    shape.localLights = settings.localLights != LocalLightingQuality::Disabled;
    shape.localShadows =
        settings.localLights != LocalLightingQuality::Disabled && settings.localShadows != LocalShadowQuality::Off;
    // The pass also skips frames whose scene has no resident probe.
    shape.perPixelReflectionProbes = leadPlan.perPixelReflectionProbes;
    // Only a leading default view culls the cascades on the GPU.
    shape.gpuShadowCulling = views.front().view == 0 && leadPlan.gpuShadowCulling;
    shape.irradianceVolume = settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume;
    shape.particles = settings.particles != ParticleQuality::Off;
    shape.views.clear();
    for (const MeshPassViewRequest& view : views) {
        const RenderSettings& viewSettings = *view.settings;
        const MeshPassFramePlan& plan = view.plan;
        shape.views.push_back(
            {.slot = view.view,
             .width = view.extent.width,
             .height = view.extent.height,
             .offscreen = view.view != 0 || defaultViewOffscreen,
             .antialiasing = viewSettings.antialiasing,
             .ambientOcclusion = plan.ambientOcclusion,
             .bloom = viewSettings.bloom,
             .exposure = viewSettings.exposure.mode,
             .environmentBackground = imageBasedLighting && viewSettings.environmentBackground,
             .depthPrepass = plan.depthPrepass,
             .gpuDriven = plan.gpuDrivenSubmission,
             .screenSpaceReflections = plan.screenSpaceReflections,
             .volumetricFog = viewSettings.volumetricFog != VolumetricFogQuality::Off,
             .motionBlur = viewSettings.motionBlur != MotionBlurQuality::Off,
             .depthOfField = viewSettings.depthOfField.quality != DepthOfFieldQuality::Off,
             .colorGrade = color_grading::lutRequested(viewSettings.colorLut) ||
                           color_grading::vignetteActive(viewSettings.vignette) ||
                           color_grading::filmGrainActive(viewSettings.filmGrain),
             // Mirrors the spatialUpscale decision in PostProcessPass::record.
             .spatialUpscale = viewSettings.upscaler.mode == UpscalerMode::Fsr1 && capabilities.spatialUpscaling,
             .decals = viewSettings.decals != DecalQuality::Off,
             .idBuffer = !view.picks.empty()});
    }
}

// The debug lines a view draws: every view's and its own. The graph shape
// and recordDebugDraw must agree on this.
bool viewDrawsDebugLines(const DebugDraw& shared, const DebugDraw& own) {
    return shared.lineCount() != 0 || own.lineCount() != 0;
}

VkExtent2D clampedViewExtent(uint32_t width, uint32_t height) {
    return {std::clamp(width, 16u, 8192u), std::clamp(height, 16u, 8192u)};
}
} // namespace

bool Renderer::init(const Window& window, RenderRuntime& rendering, FrameTaskExecutor& frameTasks,
                    const RendererOptions& options) {
    m_rendering = &rendering;
    m_frameTasks = &frameTasks;
    m_options = options;
    m_pipelineCreationMilliseconds = 0.0;
    m_pipelineFirstUseMilliseconds = 0.0;
    m_prewarmedPipelineCount = 0;
    m_pipelineCacheWarm = false;
    m_pipelineFirstUsePending = true;
    applyDiagnosticOverrides(rendering);
    if (!initContext(window, options.validation)) {
        return false;
    }
    rendering.m_capabilities = detectRenderCapabilities(m_context.pDevice());
    rendering.m_capabilities.imageBasedLighting = true;
    rendering.m_capabilities.reflectionProbes = true;
    // The probe cube array needs imageCubeArray, which device selection requires.
    rendering.m_capabilities.perPixelReflectionProbes = true;
    rendering.m_capabilities.irradianceVolumes = true;
    rendering.m_capabilities.dedicatedTransferQueue = m_context.hasDedicatedTransferQueue();
    rendering.m_capabilities.timelineTransfers = m_context.timelineSemaphoreSupported();
    rendering.m_capabilities.wireframe = m_context.fillModeNonSolidSupported();
    if (!createSwapchainResources(window)) {
        return false;
    }
    if (!m_uiPass.init(m_context, window, m_swapchain.format(), m_swapchain.imageCount())) {
        return false;
    }
    if (!m_uiImages.init(m_context, m_uiPass)) {
        return false;
    }
    if (!m_viewportTarget.init(m_context, m_uiPass, m_swapchain.format(), m_swapchain.depthFormat(), framesInFlight,
                               m_swapchain.extent())) {
        return false;
    }
    m_pipelineCacheWarm = m_context.pipelineCacheWarm();
    const auto pipelineCreationBegin = std::chrono::steady_clock::now();
    if (!m_meshPass.init(m_context, m_swapchain.format(), m_swapchain.depthFormat(), framesInFlight,
                         m_viewportTarget.extent(), m_viewportTarget.depthView())) {
        return false;
    }
    m_pipelineCreationMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipelineCreationBegin).count();
    if (!m_debugDrawPass.init(m_context, m_swapchain.format(), framesInFlight)) {
        return false;
    }
    m_prewarmedPipelineCount = m_meshPass.prewarmedPipelineCount();
    m_meshPass.setFixedFrameSeconds(options.fixedFrameSeconds);
    m_meshPass.setDiagnosticOverrides(MeshPassDiagnosticOverrides::fromEnvironment());
    rendering.m_capabilities.gpuDrivenPath = m_meshPass.gpuDrivenSupported();
    rendering.m_capabilities.maximumLocalLights = m_meshPass.localLightCapacity();
    rendering.m_capabilities.volumetricFog = m_meshPass.volumetricFogSupported();
    rendering.m_capabilities.spatialUpscaling = m_meshPass.spatialUpscaleSupported();
    m_currentFrame = 0;
    m_submittedFrames = 0;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_context.pDevice(), &properties);
    m_gpuName = properties.deviceName;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_context.pDevice(), &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_context.pDevice(), &queueFamilyCount, queueFamilies.data());
    const uint32_t timestampValidBits = queueFamilies[m_context.gfxQueueIndex()].timestampValidBits;
    for (FrameResources& frame : m_frames) {
        if (!frame.init(m_context.device(), m_context.gfxQueueIndex(), properties.limits.timestampPeriod,
                        timestampValidBits)) {
            return false;
        }
    }
    return true;
}

bool Renderer::initContext(const Window& window, const GraphicsValidationOptions& validation) {
    if (!m_context.initInstance(validation, LogChannel(m_options.logger, "graphics"))) {
        return false;
    }
    if (!m_surface.init(m_context.instance(), window.handle())) {
        return false;
    }
    if (!m_context.initPhysicalDevice(m_surface)) {
        return false;
    }
    if (!m_context.initGfxQueue(m_surface)) {
        return false;
    }
    if (!m_context.initDevice()) {
        return false;
    }
    return m_context.initVMA();
}

bool Renderer::createSwapchainResources(const Window& window) {
    return m_swapchain.initSwapchain(window, m_surface, m_context, {.transferSource = m_options.frameCapture});
}

void Renderer::destroySwapchainResources() {
    m_swapchain.destroySwapchain(m_context);
}

bool Renderer::drawFrame(const Window& window, Scene& scene, const AssetManager& assets) {
    // Debug primitives last one frame, drawn or not.
    struct DebugDrawLifetime {
        Renderer& renderer;

        ~DebugDrawLifetime() {
            renderer.clearDebugDraw();
        }
    } debugDrawLifetime{*this};

    applyDiagnosticOverrides(*m_rendering);
    const FrameStatus surfaceStatus = prepareSurface(window);
    if (surfaceStatus != FrameStatus::Ready) {
        m_uiPass.cancelFrame();
        return surfaceStatus == FrameStatus::Skipped;
    }
    if (m_viewportFailed) {
        m_uiPass.cancelFrame();
        return false;
    }
    FrameResources& frame = m_frames[m_currentFrame];
    const auto frameWaitBegin = std::chrono::steady_clock::now();
    if (!frame.waitUntilAvailable(m_context.device())) {
        m_uiPass.cancelFrame();
        return false;
    }
    const double frameWaitMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameWaitBegin).count();
    collectPicks();
    GpuFrameTimings gpuTimings;
    if (!frame.collectTimings(m_context.device(), gpuTimings)) {
        m_uiPass.cancelFrame();
        return false;
    }
    pruneUnusedUiTextures(assets);
    // This slot's previous submission, and every earlier one, has completed.
    const uint64_t recordingSerial = m_submittedFrames + 1;
    m_uiImages.collect(recordingSerial > framesInFlight ? recordingSerial - framesInFlight : 0);
    // Replacing this slot's leases is safe after its fence signals; assets used
    // by the previous submission can now be collected without a queue idle.
    m_frameAssets[m_currentFrame] = assets.leaseSceneAssets(scene);
    m_frameAssets[m_currentFrame].fonts.insert(m_frameAssets[m_currentFrame].fonts.end(),
                                               m_pendingUiAssets.fonts.begin(), m_pendingUiAssets.fonts.end());
    m_frameAssets[m_currentFrame].meshes.insert(m_frameAssets[m_currentFrame].meshes.end(),
                                                m_pendingUiAssets.meshes.begin(), m_pendingUiAssets.meshes.end());
    m_frameAssets[m_currentFrame].textures.insert(m_frameAssets[m_currentFrame].textures.end(),
                                                  m_pendingUiAssets.textures.begin(), m_pendingUiAssets.textures.end());
    m_pendingUiAssets.clear();
    uint32_t imageIndex = 0;
    const auto acquireBegin = std::chrono::steady_clock::now();
    const FrameStatus acquireStatus = acquireImage(frame, imageIndex);
    const double acquireMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireBegin).count();
    if (acquireStatus != FrameStatus::Ready) {
        m_uiPass.cancelFrame();
        return acquireStatus == FrameStatus::Skipped;
    }
    if (m_captureRequested) {
        prepareCapture();
    }
    const auto recordBegin = std::chrono::steady_clock::now();
    if (!recordFrame(frame, imageIndex, scene, assets)) {
        failFramePicks("The frame that would draw the pick failed to record");
        m_uiPass.cancelFrame();
        return false;
    }
    const double recordMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordBegin).count();
    if (m_pipelineFirstUsePending) {
        m_pipelineFirstUseMilliseconds = recordMilliseconds;
        m_pipelineFirstUsePending = false;
    }
    if (!frame.submit(m_context.device(), m_context.gfxQueue(), m_swapchain.renderComplete(imageIndex),
                      m_meshPass.uploadTimelineSemaphore(), m_meshPass.requiredUploadTimelineValue(),
                      m_meshPass.textureUploadTimelineSemaphore(), m_meshPass.requiredTextureUploadTimelineValue())) {
        failFramePicks("The frame that would draw the pick failed to submit");
        return false;
    }
    const uint64_t submittedFrame = ++m_submittedFrames;
    m_slotSubmittedFrames[m_currentFrame] = submittedFrame;
    submitPicks(submittedFrame);
    for (RenderRuntime::ViewSlot& view : m_rendering->m_views) {
        if (view.stats.rendered) {
            view.stats.submittedFrame = submittedFrame;
        }
    }
    // The leading view wrote the frame's per-view timestamps.
    // Reduced for the view shading mode, as recorded (see ScopedViewShadingSettings).
    const RenderSettings timedSettings =
        viewShadingFrameSettings(*m_viewRequests.front().settings, m_rendering->m_capabilities);
    frame.markSubmitted(
        submittedFrame, timedSettings.bloom != BloomQuality::Off, timedSettings.antialiasing == AntialiasingMode::Taa,
        timedSettings.ambientOcclusion != AmbientOcclusionQuality::Off,
        timedSettings.reflections != ReflectionQuality::ProbesOnly, m_rendering->m_stats.uploadBytes != 0);
    if (m_capturePrepared) {
        finishCapture(frame);
    }
    const auto presentBegin = std::chrono::steady_clock::now();
    const bool presented = presentFrame(imageIndex);
    const double presentMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentBegin).count();
    RenderStats& stats = m_rendering->m_stats;
    stats.submittedFrame = submittedFrame;
    stats.frameSubmitted = true;
    stats.cpuFrameWaitMilliseconds = frameWaitMilliseconds;
    stats.cpuAcquireMilliseconds = acquireMilliseconds;
    stats.cpuPresentMilliseconds = presentMilliseconds;
    stats.gpuTimingsValid = gpuTimings.valid;
    stats.gpuSampleFrame = gpuTimings.frame;
    stats.gpuFrameMilliseconds = gpuTimings.frameMilliseconds;
    stats.gpuShadowMilliseconds = gpuTimings.shadowMilliseconds;
    stats.gpuOpaqueMilliseconds = gpuTimings.opaqueMilliseconds;
    stats.gpuBlendedMilliseconds = gpuTimings.blendedMilliseconds;
    stats.gpuSceneMilliseconds = gpuTimings.sceneMilliseconds;
    stats.gpuBloomMilliseconds = gpuTimings.bloomMilliseconds;
    stats.gpuPostMilliseconds = gpuTimings.postMilliseconds;
    stats.gpuUiMilliseconds = gpuTimings.uiMilliseconds;
    stats.gpuCullingMilliseconds = gpuTimings.cullingMilliseconds;
    stats.gpuTemporalMilliseconds = gpuTimings.temporalMilliseconds;
    stats.gpuAmbientOcclusionMilliseconds = gpuTimings.ambientOcclusionMilliseconds;
    stats.gpuReflectionsMilliseconds = gpuTimings.reflectionsMilliseconds;
    stats.gpuExposureMilliseconds = gpuTimings.exposureMilliseconds;
    stats.gpuDepthPrepassMilliseconds = gpuTimings.depthPrepassMilliseconds;
    stats.gpuDepthPyramidMilliseconds = gpuTimings.depthPyramidMilliseconds;
    stats.gpuLightClusterMilliseconds = gpuTimings.lightClusterMilliseconds;
    stats.gpuEnvironmentPrecomputeMilliseconds = gpuTimings.environmentPrecomputeMilliseconds;
    stats.gpuSkyboxMilliseconds = gpuTimings.skyboxMilliseconds;
    stats.gpuShadowCullingMilliseconds = gpuTimings.shadowCullingMilliseconds;
    stats.gpuLocalShadowMilliseconds = gpuTimings.localShadowMilliseconds;
    stats.gpuReflectionProbeClusterMilliseconds = gpuTimings.reflectionProbeClusterMilliseconds;
    stats.gpuVolumetricFogMilliseconds = gpuTimings.volumetricFogMilliseconds;
    stats.gpuIrradianceVolumeMilliseconds = gpuTimings.irradianceVolumeMilliseconds;
    stats.gpuMotionBlurMilliseconds = gpuTimings.motionBlurMilliseconds;
    stats.gpuDepthOfFieldMilliseconds = gpuTimings.depthOfFieldMilliseconds;
    stats.gpuColorGradeMilliseconds = gpuTimings.colorGradeMilliseconds;
    stats.gpuFsrEasuMilliseconds = gpuTimings.fsrEasuMilliseconds;
    stats.gpuFsrRcasMilliseconds = gpuTimings.fsrRcasMilliseconds;
    stats.gpuDecalClusterMilliseconds = gpuTimings.decalClusterMilliseconds;
    stats.gpuParticleSimulationMilliseconds = gpuTimings.particleSimulationMilliseconds;
    stats.gpuParticleDrawMilliseconds = gpuTimings.particleDrawMilliseconds;
    stats.gpuTransmissionBackgroundMilliseconds = gpuTimings.transmissionBackgroundMilliseconds;
    stats.gpuUploadWaitMilliseconds = gpuTimings.uploadWaitMilliseconds;
    const StreamingStats& streaming = assets.streaming().stats();
    stats.streamingDecodedBytes = streaming.decodedBytes;
    stats.streamingUploadedBytes = streaming.uploadedBytes;
    stats.streamingDecodeBacklogBytes = streaming.decodeBacklogBytes;
    stats.streamingUploadBacklogBytes = streaming.uploadBacklogBytes;
    stats.streamingCpuPeakBytes = streaming.peakCpuResidentBytes;
    stats.streamingGpuPeakBytes = streaming.peakGpuResidentBytes;
    stats.streamingResidencyMisses = streaming.residencyMisses;
    stats.streamingHardBudgetFailures = streaming.hardBudgetFailures;
    stats.pipelineCacheWarm = m_pipelineCacheWarm;
    stats.pipelineCreationEvents = m_prewarmedPipelineCount;
    stats.pipelineCreationMilliseconds = m_pipelineCreationMilliseconds;
    stats.pipelineFirstUseEvents = m_prewarmedPipelineCount;
    stats.pipelineFirstUseMilliseconds = m_pipelineFirstUseMilliseconds;
    m_currentFrame = (m_currentFrame + 1) % framesInFlight;
    return presented;
}

uint64_t Renderer::gpuAllocationBytes() const {
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(m_context.vmaAllocator(), &statistics);
    return statistics.total.statistics.allocationBytes;
}

Renderer::FrameStatus Renderer::prepareSurface(const Window& window) {
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window.handle(), &width, &height)) {
        m_context.log().error(std::string("Cannot query drawable size: ") + SDL_GetError());
        return FrameStatus::Failed;
    }
    const bool minimized = (SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_MINIMIZED) != 0;
    const bool hasDrawableSize = width > 0 && height > 0;
    if (minimized || !hasDrawableSize) {
        SDL_Delay(surfaceRetryDelayMs);
        return FrameStatus::Skipped;
    }
    if (!m_resizeRequested) {
        return FrameStatus::Ready;
    }
    const VkSurfaceCapabilitiesKHR caps = m_surface.capabilities(m_context.pDevice());
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        SDL_Delay(surfaceRetryDelayMs);
        return FrameStatus::Skipped;
    }
    if (!recreateSwapchain(window)) {
        return FrameStatus::Failed;
    }
    return FrameStatus::Ready;
}

bool Renderer::recreateSwapchain(const Window& window) {
    if (!checkVulkan(vkDeviceWaitIdle(m_context.device()), "Wait before resize")) {
        return false;
    }
    const VkExtent2D previousExtent = m_swapchain.extent();
    destroySwapchainResources();
    if (!createSwapchainResources(window)) {
        return false;
    }
    const VkExtent2D extent = m_swapchain.extent();
    const bool resized = extent.width != previousExtent.width || extent.height != previousExtent.height;
    if (!m_rendering->m_views.front().sizeRequested) {
        // Picks of the swapchain-sized default view address the old size.
        if (resized) {
            m_rendering->m_picks.cancelView(m_rendering->defaultView());
        }
        if (!m_viewportTarget.resize(m_context, m_uiPass, m_swapchain.extent()) ||
            !m_meshPass.resizeView(m_context, 0, m_viewportTarget.extent(), m_viewportTarget.depthView())) {
            return false;
        }
    }
    m_resizeRequested = false;
    return true;
}

bool Renderer::prepareViewport() {
    const RenderRuntime::ViewSlot& view = m_rendering->m_views.front();
    if (!view.sizeRequested) {
        return true;
    }
    const VkExtent2D requested = clampedViewExtent(view.requestedWidth, view.requestedHeight);
    if (requested.width == m_viewportTarget.extent().width && requested.height == m_viewportTarget.extent().height) {
        return true;
    }
    if (!checkVulkan(vkDeviceWaitIdle(m_context.device()), "Wait before viewport resize")) {
        return false;
    }
    m_rendering->m_picks.cancelView(m_rendering->defaultView());
    return m_viewportTarget.resize(m_context, m_uiPass, requested) &&
           m_meshPass.resizeView(m_context, 0, requested, m_viewportTarget.depthView());
}

bool Renderer::prepareViews() {
    if (!prepareViewport()) {
        return false;
    }
    std::vector<RenderRuntime::ViewSlot>& slots = m_rendering->m_views;
    if (m_viewTargets.size() < slots.size()) {
        m_viewTargets.resize(slots.size());
    }
    // Targets of destroyed, replaced, or resized views may still be read by
    // submitted frames, so they change only after one device idle.
    const auto wanted = [&](uint32_t slot) {
        return slot < slots.size() && slots[slot].alive && slots[slot].sizeRequested;
    };
    bool idleNeeded = false;
    for (uint32_t slot = 1; slot < m_viewTargets.size(); ++slot) {
        const ViewTargets* targets = m_viewTargets[slot].get();
        if (targets == nullptr) {
            continue;
        }
        if (!wanted(slot) || targets->generation != slots[slot].generation) {
            idleNeeded = true;
            continue;
        }
        const VkExtent2D requested = clampedViewExtent(slots[slot].requestedWidth, slots[slot].requestedHeight);
        const VkExtent2D current = targets->target.extent();
        idleNeeded = idleNeeded || requested.width != current.width || requested.height != current.height;
    }
    if (idleNeeded && !checkVulkan(vkDeviceWaitIdle(m_context.device()), "Wait before render view changes")) {
        return false;
    }
    for (uint32_t slot = 1; slot < m_viewTargets.size(); ++slot) {
        std::unique_ptr<ViewTargets>& targets = m_viewTargets[slot];
        if (targets && (!wanted(slot) || targets->generation != slots[slot].generation)) {
            destroyViewTargets(*targets, slot);
            targets.reset();
        }
        if (!wanted(slot)) {
            continue;
        }
        RenderRuntime::ViewSlot& view = slots[slot];
        const VkExtent2D requested = clampedViewExtent(view.requestedWidth, view.requestedHeight);
        if (!targets) {
            targets = std::make_unique<ViewTargets>();
            targets->generation = view.generation;
            if (!targets->target.init(m_context, m_uiPass, m_swapchain.format(), m_swapchain.depthFormat(),
                                      framesInFlight, requested) ||
                !m_meshPass.createView(m_context, slot, requested, targets->target.depthView())) {
                m_context.log().error("Could not create render view " + std::to_string(slot) + " at " +
                                      std::to_string(requested.width) + "x" + std::to_string(requested.height));
                destroyViewTargets(*targets, slot);
                targets.reset();
                return false;
            }
        } else if (requested.width != targets->target.extent().width ||
                   requested.height != targets->target.extent().height) {
            m_rendering->m_picks.cancelView({.index = slot, .generation = view.generation});
            if (!targets->target.resize(m_context, m_uiPass, requested) ||
                !m_meshPass.resizeView(m_context, slot, requested, targets->target.depthView())) {
                m_context.log().error("Could not resize render view " + std::to_string(slot));
                return false;
            }
        }
        view.texture = {targets->target.texture(m_currentFrame)};
        view.width = targets->target.extent().width;
        view.height = targets->target.extent().height;
    }
    return true;
}

void Renderer::destroyViewTargets(ViewTargets& targets, uint32_t slot) {
    m_meshPass.destroyView(m_context, slot);
    targets.target.shutdown(m_context, m_uiPass);
}

Renderer::FrameStatus Renderer::acquireImage(const FrameResources& frame, uint32_t& imageIndex) {
    const VkResult result = m_swapchain.acquire(m_context.device(), frame.imageAcquired(), imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        m_resizeRequested = true;
        return FrameStatus::Skipped;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        m_resizeRequested = true;
        return FrameStatus::Ready;
    }
    if (!checkVulkan(result, "Acquire image")) {
        return FrameStatus::Failed;
    }
    return FrameStatus::Ready;
}

bool Renderer::recordFrame(const FrameResources& frame, uint32_t imageIndex, const Scene& scene,
                           const AssetManager& assets) {
    if (!frame.beginRecording()) {
        return false;
    }
    // Before the UI pass samples them; UI draw data of this frame still uses
    // the textures published by earlier frames.
    m_uiImages.sync(frame.commandBuffer(), m_submittedFrames + 1, *m_rendering);
    // View shading: the plan, the graph shape and every recorded pass read the
    // shading mode's reduced settings until this frame is recorded.
    const ScopedViewShadingSettings viewShading{*m_rendering};
    const VkImage image = m_swapchain.swapchainImages()[imageIndex];
    const VkImageView view = m_swapchain.swapchainImageViews()[imageIndex];
    const bool defaultViewOffscreen = m_rendering->m_views.front().sizeRequested;
    // One plan per view drives both the descriptive graph and the recorded passes.
    collectViewRequests(image, view);
    attachPicks();
    if (!updateRenderGraphPlan()) {
        return false;
    }
    if (!m_meshPass.record(frame.commandBuffer(), m_currentFrame, scene, assets, *m_rendering, m_viewRequests,
                           *m_frameTasks, frame.timestampQueries())) {
        return false;
    }
    if (!recordDebugDraw(frame.commandBuffer())) {
        return false;
    }
    RenderStats& graphStats = m_rendering->m_stats;
    graphStats.renderGraphBarrierCount = m_renderGraphStatistics.barrierCount;
    graphStats.renderGraphQueueTransfers = m_renderGraphStatistics.queueTransferCount;
    graphStats.transientRequestedBytes = m_renderGraphStatistics.requestedTransientBytes;
    graphStats.transientCommittedBytes = m_renderGraphStatistics.committedTransientBytes;
    graphStats.transientAliasedBytes = m_renderGraphStatistics.aliasedTransientBytes;
    graphStats.transientPeakBytes = m_renderGraphStatistics.peakTransientBytes;
    if (!m_swapchainToUiBarrier) {
        return false;
    }
    for (size_t index = 0; index < m_viewRequests.size(); ++index) {
        const MeshPassViewRequest& request = m_viewRequests[index];
        if (request.colorImage == image) {
            continue;
        }
        if (!m_viewToUiBarriers[index]) {
            return false;
        }
        applyGraphBarrier(frame.commandBuffer(), request.colorImage, *m_viewToUiBarriers[index]);
        if (request.view == 0) {
            m_viewportTarget.markWritten(m_currentFrame);
        } else {
            m_viewTargets[request.view]->target.markWritten(m_currentFrame);
        }
    }
    applyGraphBarrier(frame.commandBuffer(), image, *m_swapchainToUiBarrier);
    const auto uiCpuBegin = std::chrono::steady_clock::now();
    if (!m_uiPass.record(frame.commandBuffer(), image, view, m_swapchain.extent(), defaultViewOffscreen)) {
        return false;
    }
    m_rendering->m_stats.cpuUiMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiCpuBegin).count();
    if (m_capturePrepared) {
        m_readback.record(frame.commandBuffer(), image);
    }
    frame.writeTimestamp(5, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    return frame.endRecording();
}

void Renderer::collectViewRequests(VkImage swapchainImage, VkImageView swapchainView) {
    std::vector<RenderRuntime::ViewSlot>& slots = m_rendering->m_views;
    // The default view always renders; a created view renders when its size
    // was requested since the previous frame and its targets match it.
    m_renderedViews.clear();
    for (uint32_t slot = 0; slot < slots.size(); ++slot) {
        RenderRuntime::ViewSlot& view = slots[slot];
        view.stats = {};
        if (slot == 0) {
            m_renderedViews.push_back(slot);
            continue;
        }
        const ViewTargets* targets = slot < m_viewTargets.size() ? m_viewTargets[slot].get() : nullptr;
        const bool ready =
            view.alive && view.renderRequested && targets != nullptr && targets->generation == view.generation;
        view.renderRequested = false;
        if (ready) {
            m_renderedViews.push_back(slot);
        }
    }
    const RenderViewId active = m_rendering->activeView();
    orderRenderedViews(m_renderedViews, m_rendering->viewAlive(active) ? active.index : 0u);
    const bool defaultViewOffscreen = slots.front().sizeRequested;
    m_viewRequests.clear();
    for (uint32_t slot : m_renderedViews) {
        RenderRuntime::ViewSlot& view = slots[slot];
        MeshPassViewRequest request{.view = slot,
                                    .cameraOverride = view.cameraOverride,
                                    .grid = view.grid,
                                    .outlinedObjects = view.outlinedObjects,
                                    .stats = &view.stats};
        if (slot == 0) {
            request.colorImage = defaultViewOffscreen ? m_viewportTarget.image(m_currentFrame) : swapchainImage;
            request.colorView = defaultViewOffscreen ? m_viewportTarget.view(m_currentFrame) : swapchainView;
            request.colorOldLayout =
                defaultViewOffscreen ? m_viewportTarget.oldLayout(m_currentFrame) : VK_IMAGE_LAYOUT_UNDEFINED;
            request.depthImage = m_viewportTarget.depthImage();
            request.depthView = m_viewportTarget.depthView();
            request.extent = defaultViewOffscreen ? m_viewportTarget.extent() : m_swapchain.extent();
        } else {
            const ViewportTarget& target = m_viewTargets[slot]->target;
            request.colorImage = target.image(m_currentFrame);
            request.colorView = target.view(m_currentFrame);
            request.colorOldLayout = target.oldLayout(m_currentFrame);
            request.depthImage = target.depthImage();
            request.depthView = target.depthView();
            request.extent = target.extent();
        }
        // Views without an override read the frame settings in place.
        if (view.settings) {
            m_composedViewSettings[slot] = composeViewSettings(m_rendering->m_settings, view.settings);
            request.settings = &m_composedViewSettings[slot];
        } else {
            request.settings = &m_rendering->m_settings;
        }
        request.plan = m_meshPass.planFrame(*request.settings, m_rendering->capabilities());
        if (view.visibleObjects) {
            request.visibleObjects = &*view.visibleObjects;
        }
        if (slot != 0 || view.visibleObjects) {
            // GPU-driven submission and its occlusion history belong to the
            // default view, which falls back to CPU submission while
            // isolated: GPU culling would draw every record.
            request.plan.gpuDrivenSubmission = false;
            request.plan.gpuShadowCulling = false;
        }
        m_viewRequests.push_back(request);
    }
}

bool Renderer::recordDebugDraw(VkCommandBuffer commandBuffer) {
    std::vector<RenderRuntime::ViewSlot>& slots = m_rendering->m_views;
    for (RenderRuntime::ViewSlot& slot : slots) {
        slot.debugLabels.clear();
    }
    const DebugDraw& shared = m_rendering->m_debugDraw;
    m_debugDrawViews.clear();
    for (const MeshPassViewRequest& request : m_viewRequests) {
        RenderRuntime::ViewSlot& slot = slots[request.view];
        const std::optional<MeshPassViewCamera> camera = m_meshPass.recordedCamera(request.view);
        if (!camera) {
            m_context.log().error("Render view " + std::to_string(request.view) + " recorded without a camera");
            return false;
        }
        const glm::vec2 viewportSize{static_cast<float>(request.extent.width),
                                     static_cast<float>(request.extent.height)};
        projectDebugLabels(shared.labels(), camera->viewProjection, viewportSize, slot.debugLabels);
        projectDebugLabels(slot.debugDraw.labels(), camera->viewProjection, viewportSize, slot.debugLabels);
        if (!viewDrawsDebugLines(shared, slot.debugDraw)) {
            continue;
        }
        m_debugDrawViews.push_back({.colorView = request.colorView,
                                    .extent = request.extent,
                                    .depthView = request.depthView,
                                    .renderExtent = camera->renderExtent,
                                    .viewProjection = camera->viewProjection,
                                    .nearPlane = camera->nearPlane,
                                    .farPlane = camera->farPlane,
                                    .lines = &slot.debugDraw});
    }
    return m_debugDrawPass.record(commandBuffer, m_currentFrame, shared, m_debugDrawViews);
}

void Renderer::clearDebugDraw() {
    if (m_rendering == nullptr) {
        return;
    }
    m_rendering->m_debugDraw.clear();
    for (RenderRuntime::ViewSlot& slot : m_rendering->m_views) {
        slot.debugDraw.clear();
    }
}

bool Renderer::updateRenderGraphPlan() {
    const bool defaultViewOffscreen = m_rendering->m_views.front().sizeRequested;
    // Filled in place so a steady frame reuses the shape's storage.
    RenderGraphShape& shape = m_frameGraphShape;
    fillRenderGraphShape(m_viewRequests, defaultViewOffscreen, m_rendering->settings(), m_rendering->capabilities(),
                         shape);
    for (size_t index = 0; index < m_viewRequests.size(); ++index) {
        shape.views[index].debugDraw =
            viewDrawsDebugLines(m_rendering->m_debugDraw, m_rendering->m_views[m_viewRequests[index].view].debugDraw);
    }
    if (shape != m_renderGraphShape) {
        RenderGraph graph;
        const auto image = [](uint32_t format, VkExtent2D extent, uint64_t usage, uint64_t bytes) {
            return RenderGraphImageDescription{.format = format,
                                               .width = extent.width,
                                               .height = extent.height,
                                               .usage = usage,
                                               .requestedBytes = bytes,
                                               .alignment = 256};
        };
        const VkExtent2D outputExtent = m_swapchain.extent();
        const auto swapchain =
            graph.importImage("swapchain", image(m_swapchain.format(), outputExtent, 1, 0), VK_IMAGE_LAYOUT_UNDEFINED);
        // Resources every view shares, written once per frame.
        const auto shadow = graph.importImage("shadow atlas", image(VK_FORMAT_D32_SFLOAT, outputExtent, 5, 0),
                                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        const auto probeCache =
            graph.importImage("reflection probe cache", image(PostProcessPass::sceneFormat, outputExtent, 5, 0),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const RenderGraphQueue uploadQueue = m_rendering->capabilities().dedicatedTransferQueue
                                                 ? RenderGraphQueue::Transfer
                                                 : RenderGraphQueue::Graphics;
        const auto meshAssets =
            graph.importBuffer("mesh asset buffers", {.size = 0, .usage = 1, .alignment = 256}, uploadQueue);
        const auto textureAssets =
            graph.importImage("texture asset images", image(VK_FORMAT_R8G8B8A8_UNORM, outputExtent, 5, 0),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, uploadQueue);
        // Persistent renderer resources the scene passes exchange. Their sizes
        // are owned by the passes, so the model records zero bytes for them.
        const auto environmentMaps =
            graph.importImage("environment maps", image(PostProcessPass::sceneFormat, outputExtent, 5, 0),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Persistent: cached tiles keep their depth across frames.
        const auto localShadowAtlas =
            graph.importImage("local shadow atlas", image(m_swapchain.depthFormat(), outputExtent, 5, 0),
                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        const auto gpuDrivenDraws = graph.importBuffer("GPU-driven draws", {.size = 0, .usage = 1, .alignment = 256});
        const RenderGraphAccess uploadAccesses[]{
            {meshAssets, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED},
            {textureAssets, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}};
        graph.addPass("asset uploads", uploadQueue, uploadAccesses);
        constexpr uint64_t geometryStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        constexpr uint64_t geometryAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        constexpr uint64_t computeReadWrite =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (shape.imageBasedLighting) {
            // Records work only on frames whose environment inputs changed.
            const RenderGraphAccess environmentAccesses[]{
                {environmentMaps, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL},
                {textureAssets, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            graph.addPass("environment precompute", RenderGraphQueue::Graphics, environmentAccesses);
            if (shape.perPixelReflectionProbes) {
                // Copies newly resident probes into the probe cache slots.
                const RenderGraphAccess residencyAccesses[]{
                    {environmentMaps, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
                    {probeCache, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}};
                graph.addPass("reflection probe residency", RenderGraphQueue::Graphics, residencyAccesses);
            }
        }
        // Irradiance volume (U6): rewrites the probe buffer only when the bound
        // volume changes; fog injection and opaque and blended shading read it.
        const auto irradianceProbes =
            graph.importBuffer("irradiance volume probes", {.size = 0, .usage = 1, .alignment = 256});
        if (shape.irradianceVolume) {
            const RenderGraphAccess irradianceUploadAccesses[]{
                {irradianceProbes, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("irradiance volume upload", RenderGraphQueue::Graphics, irradianceUploadAccesses);
        }
        if (shape.shadows) {
            std::vector<RenderGraphAccess> shadowAccesses{
                {shadow, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL},
                {meshAssets, RenderGraphAccessMode::Read, geometryStages, geometryAccess, VK_IMAGE_LAYOUT_UNDEFINED}};
            if (shape.gpuShadowCulling) {
                // Dirty cascades cull into their own GPU-driven regions first.
                const RenderGraphAccess shadowCullAccesses[]{
                    {gpuDrivenDraws, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass("GPU culling (shadow cascades)", RenderGraphQueue::Graphics, shadowCullAccesses);
                shadowAccesses.push_back({gpuDrivenDraws, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                          VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
            }
            graph.addPass("directional shadows", RenderGraphQueue::Graphics, shadowAccesses);
        }
        if (shape.localShadows) {
            // Re-renders only the atlas tiles the update budget scheduled.
            const RenderGraphAccess localShadowAccesses[]{
                {localShadowAtlas, RenderGraphAccessMode::ReadWrite,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL},
                {meshAssets, RenderGraphAccessMode::Read, geometryStages, geometryAccess, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("local light shadows", RenderGraphQueue::Graphics, localShadowAccesses);
        }
        // Persistent particle pool, sort keys, and indirect draw arguments owned
        // by ParticlePass; the passes record work only when the scene has emitters.
        const auto particlePool = graph.importBuffer("particle pool", {.size = 0, .usage = 1, .alignment = 256});
        std::vector<RenderGraphResourceHandle> viewOutputs;
        viewOutputs.reserve(shape.views.size());
        for (size_t viewIndex = 0; viewIndex < shape.views.size(); ++viewIndex) {
            const RenderGraphViewShape& view = shape.views[viewIndex];
            // The default view keeps the single-view resource and pass names.
            const std::string prefix = view.slot == 0 ? std::string{} : "view " + std::to_string(view.slot) + ": ";
            const auto name = [&](const char* resource) { return prefix + resource; };
            const VkExtent2D extent{view.width, view.height};
            const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
            const auto viewImage = [&](uint32_t format, uint64_t usage, uint64_t bytes) {
                return image(format, extent, usage, bytes);
            };
            const auto output = view.offscreen
                                    ? graph.importImage(name("editor viewport"), viewImage(m_swapchain.format(), 3, 0),
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                    : swapchain;
            viewOutputs.push_back(output);
            const auto depth = graph.importImage(name("scene depth"), viewImage(m_swapchain.depthFormat(), 5, 0),
                                                 VK_IMAGE_LAYOUT_UNDEFINED);
            const auto history =
                graph.importImage(name("temporal history"), viewImage(TemporalResolvePass::historyFormat, 5, 0),
                                  TemporalResolvePass::resolvedLayout);
            const auto lightClusters =
                graph.importBuffer(name("light clusters"), {.size = 0, .usage = 1, .alignment = 256});
            const auto probeClusters =
                graph.importBuffer(name("reflection probe clusters"), {.size = 0, .usage = 1, .alignment = 256});
            const auto decalClusters =
                graph.importBuffer(name("decal clusters"), {.size = 0, .usage = 1, .alignment = 256});
            const auto normals = graph.importImage(
                name("scene normals"), viewImage(PostProcessPass::normalFormat, 5, 0), VK_IMAGE_LAYOUT_UNDEFINED);
            // Written by the prepass only on frames that trace screen-space reflections.
            const auto specular = graph.importImage(
                name("scene specular"), viewImage(PostProcessPass::specularFormat, 5, 0), VK_IMAGE_LAYOUT_UNDEFINED);
            const auto depthPyramid = graph.importImage(name("depth pyramid"), viewImage(DepthPyramid::format, 5, 0),
                                                        VK_IMAGE_LAYOUT_GENERAL);
            const auto hdr = graph.createTransientImage(
                name("HDR scene"), viewImage(PostProcessPass::sceneFormat, 7, pixels * sizeof(uint16_t) * 4));
            const auto motion =
                graph.createTransientImage(name("motion and reactive mask"),
                                           viewImage(PostProcessPass::motionFormat, 5, pixels * sizeof(uint16_t) * 4));
            // Persistent per frame slot; AmbientOcclusionPass keeps it in GENERAL.
            const auto ambientOcclusion =
                graph.importImage(name("ambient occlusion"), viewImage(AmbientOcclusionPass::occlusionFormat, 5, 0),
                                  AmbientOcclusionPass::occlusionLayout);
            // Persistent and shared by consecutive frames, like the temporal history.
            const auto reflections =
                graph.importImage(name("screen-space reflection target"),
                                  viewImage(ScreenSpaceReflectionPass::targetFormat, 5, 0), VK_IMAGE_LAYOUT_GENERAL);
            // Persistent froxel volumes; VolumetricFogPass keeps them in GENERAL.
            const auto fogVolume =
                graph.importImage(name("volumetric fog volume"), viewImage(VolumetricFogPass::volumeFormat, 5, 0),
                                  VK_IMAGE_LAYOUT_GENERAL);
            if (view.gpuDriven) {
                // The early phase emits last frame's visible set; it binds the
                // pyramid statically but only the late phase samples it.
                const RenderGraphAccess earlyCullAccesses[]{
                    {gpuDrivenDraws, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass(name("GPU culling (early)"), RenderGraphQueue::Graphics, earlyCullAccesses);
            }
            if (view.idBuffer) {
                // One region per pick request, read back after the frame's fence.
                const auto ids = graph.importImage(name("pick id buffer"), viewImage(IdBufferPass::idFormat, 5, 0),
                                                   VK_IMAGE_LAYOUT_UNDEFINED);
                const RenderGraphAccess idBufferAccesses[]{
                    {ids, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
                graph.addPass(name("id buffer (picking)"), RenderGraphQueue::Graphics, idBufferAccesses);
            }
            if (shape.localLights) {
                const RenderGraphAccess clusterAccesses[]{
                    {lightClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass(name("light cluster assignment"), RenderGraphQueue::Graphics, clusterAccesses);
            }
            if (shape.perPixelReflectionProbes) {
                const RenderGraphAccess probeClusterAccesses[]{
                    {probeClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass(name("reflection probe cluster assignment"), RenderGraphQueue::Graphics,
                              probeClusterAccesses);
            }
            if (view.decals) {
                // Skipped on frames whose scene has no decal.
                const RenderGraphAccess decalClusterAccesses[]{
                    {decalClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass(name("decal cluster assignment"), RenderGraphQueue::Graphics, decalClusterAccesses);
            }
            // Reads of the frame's shared lighting by a compute pass of this view.
            const auto addLightingReads = [&](std::vector<RenderGraphAccess>& accesses) {
                if (shape.shadows) {
                    accesses.push_back({shadow, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
                }
                if (shape.localLights) {
                    accesses.push_back({lightClusters, RenderGraphAccessMode::Read,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED});
                }
                if (shape.irradianceVolume) {
                    accesses.push_back({irradianceProbes, RenderGraphAccessMode::Read,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED});
                }
            };
            if (view.volumetricFog) {
                // Injects the medium lit by the shadowed sun and clustered lights,
                // reprojects last frame's volume, and integrates along depth.
                std::vector<RenderGraphAccess> fogAccesses{
                    {fogVolume, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | computeReadWrite, VK_IMAGE_LAYOUT_GENERAL}};
                // Ambient in-scattering inside an irradiance volume comes from its probes.
                addLightingReads(fogAccesses);
                graph.addPass(name("volumetric fog"), RenderGraphQueue::Graphics, fogAccesses);
            }
            if (viewIndex == 0 && shape.particles) {
                // Emits, simulates, lights (sun shadows, the leading view's
                // clustered lights, ambient probes), and sorts the particles
                // once per frame for the leading view.
                std::vector<RenderGraphAccess> particleAccesses{
                    {particlePool, RenderGraphAccessMode::ReadWrite,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT | computeReadWrite, VK_IMAGE_LAYOUT_UNDEFINED}};
                addLightingReads(particleAccesses);
                graph.addPass("particle simulation", RenderGraphQueue::Graphics, particleAccesses);
            }
            const auto addDepthPyramidBuild = [&](const char* passName) {
                const RenderGraphAccess pyramidAccesses[]{
                    {depth, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                    {depthPyramid, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
                graph.addPass(name(passName), RenderGraphQueue::Graphics, pyramidAccesses);
            };
            const auto addLateCull = [&] {
                const RenderGraphAccess lateCullAccesses[]{
                    {depthPyramid, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL},
                    {gpuDrivenDraws, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     computeReadWrite, VK_IMAGE_LAYOUT_UNDEFINED}};
                graph.addPass(name("GPU culling (late)"), RenderGraphQueue::Graphics, lateCullAccesses);
            };
            const RenderGraphAccess indirectDrawRead{gpuDrivenDraws, RenderGraphAccessMode::Read,
                                                     VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                                     VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
            if (view.depthPrepass) {
                std::vector<RenderGraphAccess> prepassAccesses{
                    {depth, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL},
                    {normals, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                    {meshAssets, RenderGraphAccessMode::Read, geometryStages, geometryAccess,
                     VK_IMAGE_LAYOUT_UNDEFINED}};
                if (view.gpuDriven) {
                    prepassAccesses.push_back(indirectDrawRead);
                }
                if (view.screenSpaceReflections) {
                    prepassAccesses.push_back(
                        {specular, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
                }
                graph.addPass(name("depth and normal prepass"), RenderGraphQueue::Graphics, prepassAccesses);
                if (view.gpuDriven) {
                    // Two-phase occlusion culling runs inside the prepass.
                    addDepthPyramidBuild("occlusion depth pyramid");
                    addLateCull();
                    graph.addPass(name("depth and normal prepass (late phase)"), RenderGraphQueue::Graphics,
                                  prepassAccesses);
                }
                // Screen-space effects read this pyramid of the complete opaque depth.
                addDepthPyramidBuild("depth pyramid");
            }
            // GTAO runs between the depth prepass and shading. It always records:
            // without the prepass or with AO off it writes unoccluded values.
            std::vector<RenderGraphAccess> ambientOcclusionAccesses{
                {ambientOcclusion, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, AmbientOcclusionPass::occlusionLayout}};
            if (view.ambientOcclusion != AmbientOcclusionQuality::Off) {
                ambientOcclusionAccesses.push_back(
                    {normals, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                ambientOcclusionAccesses.push_back({depthPyramid, RenderGraphAccessMode::Read,
                                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL});
            }
            graph.addPass(name("ambient occlusion"), RenderGraphQueue::Graphics, ambientOcclusionAccesses);
            std::vector<RenderGraphAccess> opaqueAccesses{
                {ambientOcclusion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, AmbientOcclusionPass::occlusionLayout},
                {hdr, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                {motion, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                {meshAssets, RenderGraphAccessMode::Read, geometryStages, geometryAccess, VK_IMAGE_LAYOUT_UNDEFINED},
                {textureAssets, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {probeCache, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            // After a prepass, shading tests the finished opaque depth; blended
            // surfaces and outlines still test it in the same pass.
            opaqueAccesses.push_back(view.depthPrepass
                                         ? RenderGraphAccess{depth, RenderGraphAccessMode::Read,
                                                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}
                                         : RenderGraphAccess{depth, RenderGraphAccessMode::Write,
                                                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL});
            if (shape.shadows) {
                opaqueAccesses.push_back({shadow, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
            }
            if (shape.imageBasedLighting) {
                // Shading and the skybox sample the prefiltered maps.
                opaqueAccesses.push_back({environmentMaps, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
            if (shape.localShadows) {
                opaqueAccesses.push_back({localShadowAtlas, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
            }
            if (shape.localLights) {
                opaqueAccesses.push_back({lightClusters, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (shape.perPixelReflectionProbes) {
                opaqueAccesses.push_back({probeClusters, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (view.decals) {
                opaqueAccesses.push_back({decalClusters, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (view.gpuDriven) {
                opaqueAccesses.push_back(indirectDrawRead);
            }
            if (view.volumetricFog) {
                // Opaque, blended, and sky shading composite the integrated fog.
                opaqueAccesses.push_back({fogVolume, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_LAYOUT_GENERAL});
            }
            if (shape.irradianceVolume) {
                opaqueAccesses.push_back({irradianceProbes, RenderGraphAccessMode::Read,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (view.gpuDriven && !view.depthPrepass) {
                // Without a prepass two-phase culling splits the shading pass.
                graph.addPass(name("opaque scene (early phase)"), RenderGraphQueue::Graphics, opaqueAccesses);
                addDepthPyramidBuild("occlusion depth pyramid");
                addLateCull();
            }
            graph.addPass(name("opaque and blended scene"), RenderGraphQueue::Graphics, opaqueAccesses);
            if (view.screenSpaceReflections) {
                // Traces the prepass normals and depth pyramid into the
                // reflection target and blends it into the scene in place. It
                // runs inside the scene pass, after opaque surfaces and the
                // background and before blended ones, which this graph models
                // as one pass.
                const RenderGraphAccess reflectionAccesses[]{
                    {hdr, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL},
                    {normals, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {specular, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {depthPyramid, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL},
                    {reflections, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL}};
                graph.addPass(name("screen-space reflections"), RenderGraphQueue::Graphics, reflectionAccesses);
            }
            if (shape.particles) {
                // Inside the scene pass after blended surfaces, in its own
                // rendering scope: premultiplied quads over the HDR target, the
                // reactive mask in the motion target, and the opaque depth
                // read-only (tested and sampled for soft particles).
                std::vector<RenderGraphAccess> particleDrawAccesses{
                    {particlePool, RenderGraphAccessMode::Read,
                     VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                     VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED},
                    {hdr, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                    {motion, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                    {depth, RenderGraphAccessMode::Read,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                    {textureAssets, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                if (view.volumetricFog) {
                    particleDrawAccesses.push_back({fogVolume, RenderGraphAccessMode::Read,
                                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL});
                }
                graph.addPass(name("particles"), RenderGraphQueue::Graphics, particleDrawAccesses);
            }
            const bool temporalResolve = view.antialiasing == AntialiasingMode::Taa;
            if (temporalResolve) {
                // Also reads the previous frame's motion target, which is outside
                // this frame's transient lifetimes.
                const RenderGraphAccess resolveAccesses[]{
                    {hdr, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {motion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {history, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     TemporalResolvePass::resolvedLayout}};
                graph.addPass(name("temporal resolve"), RenderGraphQueue::Graphics, resolveAccesses);
            }
            // The scene the later passes read: the resolved history with TAA,
            // the HDR target otherwise, or the depth of field output when it runs.
            RenderGraphAccess sceneComputeRead =
                temporalResolve
                    ? RenderGraphAccess{history, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, TemporalResolvePass::resolvedLayout}
                    : RenderGraphAccess{hdr, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            // Persistent and shared by consecutive frames; DepthOfFieldPass
            // keeps its gather targets and output in GENERAL.
            const auto focusedScene =
                graph.importImage(name("depth of field targets"), viewImage(DepthOfFieldPass::imageFormat, 5, 0),
                                  DepthOfFieldPass::outputLayout);
            if (view.depthOfField) {
                const RenderGraphAccess depthOfFieldAccesses[]{
                    sceneComputeRead,
                    {motion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {focusedScene, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, DepthOfFieldPass::outputLayout}};
                graph.addPass(name("depth of field"), RenderGraphQueue::Graphics, depthOfFieldAccesses);
                sceneComputeRead = {focusedScene, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, DepthOfFieldPass::outputLayout};
            }
            // Persistent blurred scene and velocity tiles; MotionBlurPass keeps
            // them in GENERAL and synchronizes them across frames. Motion blur
            // reads the focused scene when depth of field runs.
            const auto motionBlurred =
                graph.importImage(name("motion blur target"), viewImage(MotionBlurPass::colorFormat, 5, 0),
                                  MotionBlurPass::blurredLayout);
            if (view.motionBlur) {
                const RenderGraphAccess motionBlurAccesses[]{
                    sceneComputeRead,
                    {motion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {motionBlurred, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, MotionBlurPass::blurredLayout}};
                graph.addPass(name("motion blur"), RenderGraphQueue::Graphics, motionBlurAccesses);
                sceneComputeRead = {motionBlurred, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, MotionBlurPass::blurredLayout};
            }
            if (view.exposure == ExposureMode::Auto) {
                graph.addPass(name("luminance histogram"), RenderGraphQueue::Graphics, {&sceneComputeRead, 1});
            }
            if (view.bloom != BloomQuality::Off) {
                graph.addPass(name("bloom chain"), RenderGraphQueue::Graphics, {&sceneComputeRead, 1});
            }
            // With a grade effect, tone mapping writes the grade's persistent
            // display target and the grade writes the output.
            const auto displayTarget =
                graph.importImage(name("colour-grade display target"), viewImage(ColorGradePass::displayFormat, 5, 0),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            // With FSR 1 the composite renders at render resolution into the
            // upscaler's persistent input target instead, and RCAS writes what
            // the composite would have.
            const auto upscaleInput = view.spatialUpscale
                                          ? graph.importImage(name("FSR input"), viewImage(m_swapchain.format(), 5, 0),
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                          : output;
            const auto gradeInput = view.colorGrade ? displayTarget : output;
            const auto toneMapTarget = view.spatialUpscale ? upscaleInput : gradeInput;
            std::vector<RenderGraphAccess> postAccesses{
                {hdr, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {depth, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {history, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, TemporalResolvePass::resolvedLayout},
                {motionBlurred, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, MotionBlurPass::blurredLayout},
                {toneMapTarget, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            if (view.depthOfField) {
                postAccesses.push_back({focusedScene, RenderGraphAccessMode::Read,
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                        DepthOfFieldPass::outputLayout});
            }
            graph.addPass(name("post-process and tone map"), RenderGraphQueue::Graphics, postAccesses);
            if (view.spatialUpscale) {
                // Persistent output-resolution EASU result; SpatialUpscalePass
                // writes it in GENERAL and RCAS samples it read-only.
                const auto upscaled =
                    graph.importImage(name("FSR upscaled scene"), viewImage(SpatialUpscalePass::upscaledFormat, 5, 0),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const RenderGraphAccess easuAccesses[]{
                    {upscaleInput, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {upscaled, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
                graph.addPass(name("FSR EASU upsample"), RenderGraphQueue::Graphics, easuAccesses);
                const RenderGraphAccess rcasAccesses[]{
                    {upscaled, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {gradeInput, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
                graph.addPass(name("FSR RCAS sharpen"), RenderGraphQueue::Graphics, rcasAccesses);
            }
            if (view.colorGrade) {
                const RenderGraphAccess gradeAccesses[]{
                    {displayTarget, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {output, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
                graph.addPass(name("colour grade"), RenderGraphQueue::Graphics, gradeAccesses);
            }
            if (view.debugDraw) {
                // DebugDrawPass: lines blend over the finished output and
                // test against the scene depth the post pass left read-only.
                const RenderGraphAccess debugDrawAccesses[]{
                    {depth, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                    {output, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
                graph.addPass(name("debug draw"), RenderGraphQueue::Graphics, debugDrawAccesses);
            }
        }
        // The UI samples every offscreen view and draws into the swapchain,
        // over the default view when that renders into the swapchain.
        std::vector<RenderGraphAccess> uiAccesses;
        bool swapchainHoldsScene = false;
        for (size_t viewIndex = 0; viewIndex < shape.views.size(); ++viewIndex) {
            if (!shape.views[viewIndex].offscreen) {
                swapchainHoldsScene = true;
                continue;
            }
            uiAccesses.push_back({viewOutputs[viewIndex], RenderGraphAccessMode::Read,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }
        uiAccesses.push_back(
            swapchainHoldsScene
                ? RenderGraphAccess{swapchain, RenderGraphAccessMode::ReadWrite,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
                : RenderGraphAccess{swapchain, RenderGraphAccessMode::Write,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        const RenderGraphPassHandle uiPass =
            graph.addPass(swapchainHoldsScene ? "runtime UI" : "editor UI", RenderGraphQueue::Graphics, uiAccesses);
        const RenderGraphCompileResult compiled = graph.compile();
        if (!compiled) {
            m_context.log().error("Could not compile render graph: " + compiled.error);
            return false;
        }
#ifndef NDEBUG
        const RenderGraphCompileResult conservative = graph.compile(true);
        if (!conservative || conservative.graph.executionOrder != compiled.graph.executionOrder ||
            conservative.graph.statistics.barrierCount < compiled.graph.statistics.barrierCount) {
            m_context.log().error("Render graph conservative synchronization validation failed");
            return false;
        }
#endif
        m_renderGraphStatistics = compiled.graph.statistics;
        m_viewToUiBarriers.assign(shape.views.size(), std::nullopt);
        m_swapchainToUiBarrier.reset();
        for (const RenderGraphBarrier& barrier : compiled.graph.barriers) {
            if (barrier.destinationPass != uiPass) {
                continue;
            }
            if (barrier.resource == swapchain) {
                m_swapchainToUiBarrier = barrier;
                // A first use of the freshly acquired image must start at the
                // stage the frame waits on the acquire semaphore, or its layout
                // transition races the presentation engine's read.
                if (static_cast<VkImageLayout>(barrier.oldLayout) == VK_IMAGE_LAYOUT_UNDEFINED) {
                    m_swapchainToUiBarrier->sourceStages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                }
                continue;
            }
            for (size_t viewIndex = 0; viewIndex < shape.views.size(); ++viewIndex) {
                if (shape.views[viewIndex].offscreen && barrier.resource == viewOutputs[viewIndex]) {
                    m_viewToUiBarriers[viewIndex] = barrier;
                }
            }
        }
        m_renderGraphShape = shape;
    }
    RenderStats& stats = m_rendering->m_stats;
    stats.renderGraphBarrierCount = m_renderGraphStatistics.barrierCount;
    stats.renderGraphQueueTransfers = m_renderGraphStatistics.queueTransferCount;
    stats.transientRequestedBytes = m_renderGraphStatistics.requestedTransientBytes;
    stats.transientCommittedBytes = m_renderGraphStatistics.committedTransientBytes;
    stats.transientAliasedBytes = m_renderGraphStatistics.aliasedTransientBytes;
    stats.transientPeakBytes = m_renderGraphStatistics.peakTransientBytes;
    return true;
}

void Renderer::attachPicks() {
    m_framePicks.clear();
    m_framePickStates.clear();
    std::vector<std::pair<size_t, size_t>> requestRanges(m_viewRequests.size());
    for (size_t requestIndex = 0; requestIndex < m_viewRequests.size(); ++requestIndex) {
        const MeshPassViewRequest& request = m_viewRequests[requestIndex];
        const RenderViewId view{.index = request.view, .generation = m_rendering->m_views[request.view].generation};
        requestRanges[requestIndex].first = m_framePicks.size();
        for (const PickRequestBook::Recording& recording : m_rendering->m_picks.startRecording(view)) {
            const std::optional<PickRect> region =
                clampPickRect(recording.rect, request.extent.width, request.extent.height);
            if (!region) {
                // Nothing of the view lies under the rectangle.
                m_rendering->m_picks.complete(
                    recording.ticket,
                    {.region = {.x = recording.rect.x, .y = recording.rect.y, .width = 0, .height = 0},
                     .viewWidth = request.extent.width,
                     .viewHeight = request.extent.height});
                continue;
            }
            auto readback = std::make_unique<IdReadback>();
            if (const std::string error = readback->prepare(m_context.vmaAllocator(), region->width, region->height);
                !error.empty()) {
                m_rendering->m_picks.fail(recording.ticket, error);
                continue;
            }
            m_framePicks.push_back({.region = *region, .readback = readback.get()});
            m_framePickStates.push_back({.ticket = recording.ticket,
                                         .region = *region,
                                         .viewExtent = request.extent,
                                         .readback = std::move(readback)});
        }
        requestRanges[requestIndex].second = m_framePicks.size() - requestRanges[requestIndex].first;
    }
    // Spans are taken once m_framePicks stopped growing.
    for (size_t requestIndex = 0; requestIndex < m_viewRequests.size(); ++requestIndex) {
        const auto [first, count] = requestRanges[requestIndex];
        m_viewRequests[requestIndex].picks = std::span(m_framePicks).subspan(first, count);
    }
}

void Renderer::submitPicks(uint64_t submittedFrame) {
    for (size_t index = 0; index < m_framePickStates.size(); ++index) {
        InFlightPick& pick = m_framePickStates[index];
        if (!m_framePicks[index].recorded) {
            m_rendering->m_picks.fail(pick.ticket, "The id buffer for the pick region could not be allocated");
            continue;
        }
        pick.objects = std::move(m_framePicks[index].objects);
        pick.viewProjection = m_framePicks[index].viewProjection;
        pick.submittedFrame = submittedFrame;
        m_inFlightPicks.push_back(std::move(pick));
    }
    m_framePicks.clear();
    m_framePickStates.clear();
}

void Renderer::failFramePicks(const std::string& error) {
    for (const InFlightPick& pick : m_framePickStates) {
        m_rendering->m_picks.fail(pick.ticket, error);
    }
    // A failed recording or submission never runs, so nothing reads the
    // readbacks.
    m_framePicks.clear();
    m_framePickStates.clear();
}

void Renderer::collectPicks() {
    if (m_inFlightPicks.empty()) {
        return;
    }
    // One queue executes frames in submission order, so a slot whose latest
    // submission finished implies every earlier frame finished too.
    uint64_t completedFrame = 0;
    for (uint32_t slot = 0; slot < framesInFlight; ++slot) {
        if (m_slotSubmittedFrames[slot] > completedFrame && m_frames[slot].completed(m_context.device())) {
            completedFrame = m_slotSubmittedFrames[slot];
        }
    }
    std::erase_if(m_inFlightPicks, [&](InFlightPick& pick) {
        if (pick.submittedFrame > completedFrame) {
            return false;
        }
        // Cancelled picks (view destroyed or resized, or the caller gave up)
        // only release their readback.
        if (!m_rendering->m_picks.awaitingResult(pick.ticket)) {
            return true;
        }
        PickResult result{.region = pick.region,
                          .viewWidth = pick.viewExtent.width,
                          .viewHeight = pick.viewExtent.height,
                          .objects = std::move(pick.objects),
                          .viewProjection = pick.viewProjection,
                          .inverseViewProjection = glm::inverse(pick.viewProjection),
                          .submittedFrame = pick.submittedFrame};
        if (const std::string error = pick.readback->read(result.ids, result.depth); !error.empty()) {
            m_rendering->m_picks.fail(pick.ticket, error);
        } else {
            m_rendering->m_picks.complete(pick.ticket, std::move(result));
        }
        return true;
    });
}

void Renderer::processUiEvent(const SDL_Event& event) {
    m_uiPass.processEvent(event);
}

void Renderer::beginUiFrame(const AssetManager& assets) {
    if (!prepareViews()) {
        m_viewportFailed = true;
    }
    if (RenderRuntime::ViewSlot& view = m_rendering->m_views.front(); view.sizeRequested) {
        view.texture = {m_viewportTarget.texture(m_currentFrame)};
        view.width = m_viewportTarget.extent().width;
        view.height = m_viewportTarget.extent().height;
    }
    m_uiPass.syncFonts(assets);
    m_uiPass.beginFrame();
}

void Renderer::drawGameUi(const GameUiContext& gameUi, const AssetManager& assets) {
    for (const UiCommand& command : gameUi.commands()) {
        if (const auto* text = std::get_if<UiTextCommand>(&command.payload)) {
            if (AssetLease<FontHandle> font = assets.lease(text->font)) {
                m_pendingUiAssets.fonts.push_back(std::move(font));
            }
        } else if (const auto* image = std::get_if<UiImageCommand>(&command.payload)) {
            if (AssetLease<TextureHandle> texture = assets.lease(image->texture)) {
                m_pendingUiAssets.textures.push_back(std::move(texture));
            }
        }
    }
    m_uiPass.drawGameUi(gameUi, [this, &assets](TextureHandle handle) {
        const uint64_t key = (static_cast<uint64_t>(handle.generation) << 32u) | handle.index;
        if (const auto found = m_gameUiTextures.find(key); found != m_gameUiTextures.end()) {
            if (assets.texture(handle) == nullptr) {
                m_uiPass.removeTexture(found->second);
                m_gameUiTextures.erase(found);
                return uint64_t{0};
            }
            return found->second;
        }
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        if (!m_meshPass.uiTexture(handle, assets, sampler, view)) {
            return uint64_t{0};
        }
        const uint64_t texture = m_uiPass.addTexture(sampler, view);
        if (texture != 0) {
            m_gameUiTextures.emplace(key, texture);
        }
        return texture;
    });
}

void Renderer::pruneUnusedUiTextures(const AssetManager& assets) {
    for (auto iterator = m_gameUiTextures.begin(); iterator != m_gameUiTextures.end();) {
        const TextureHandle handle{static_cast<uint32_t>(iterator->first),
                                   static_cast<uint32_t>(iterator->first >> 32u)};
        if (assets.texture(handle) != nullptr) {
            ++iterator;
            continue;
        }
        m_uiPass.removeTexture(iterator->second);
        iterator = m_gameUiTextures.erase(iterator);
    }
}

InputCapture Renderer::uiInputCapture() const {
    const bool viewportInput = m_rendering != nullptr && m_rendering->anyViewInputActive();
    return {.keyboard = m_uiPass.wantsKeyboardCapture() && !viewportInput,
            .mouse = m_uiPass.wantsMouseCapture() && !viewportInput};
}

FrameCaptureResult Renderer::takeFrameCapture() {
    m_captureRequested = false;
    return std::exchange(m_capture, {});
}

void Renderer::prepareCapture() {
    m_captureRequested = false;
    if (!m_options.frameCapture) {
        m_capture.error = "Frame capture was requested but the renderer was created without it";
        return;
    }
    if (const ImageFileResult result = m_readback.prepare(m_context, m_swapchain.extent(), m_swapchain.format());
        !result) {
        m_capture.error = result.error;
        return;
    }
    m_capturePrepared = true;
}

// Waiting for the frame's fence here stalls only the captured frame.
void Renderer::finishCapture(const FrameResources& frame) {
    m_capturePrepared = false;
    RgbaImage image;
    if (!frame.waitUntilAvailable(m_context.device())) {
        m_capture.error = "Waiting for the captured frame failed";
    } else if (const ImageFileResult result = m_readback.read(image); !result) {
        m_capture.error = result.error;
    } else {
        m_capture.image = std::move(image);
    }
    m_readback.release();
}

bool Renderer::presentFrame(uint32_t imageIndex) {
    const VkResult result = m_swapchain.present(m_context.gfxQueue(), imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_resizeRequested = true;
        return true;
    }
    return checkVulkan(result, "Present frame");
}

void Renderer::shutdown() {
    const VkDevice device = m_context.device();
    if (device) {
        vkDeviceWaitIdle(device);
        for (const auto& [key, texture] : m_gameUiTextures) {
            static_cast<void>(key);
            m_uiPass.removeTexture(texture);
        }
        m_gameUiTextures.clear();
        m_uiImages.shutdown();
        m_readback.release();
        // The device idles, so every recorded pick finished; publish them
        // before their readbacks go.
        collectPicks();
        m_inFlightPicks.clear();
        failFramePicks("The renderer shut down");
        for (FrameResources& frame : m_frames) {
            frame.shutdown(device);
        }
        for (uint32_t slot = 1; slot < m_viewTargets.size(); ++slot) {
            if (m_viewTargets[slot]) {
                destroyViewTargets(*m_viewTargets[slot], slot);
            }
        }
        m_viewTargets.clear();
        m_meshPass.shutdown(m_context);
        m_debugDrawPass.shutdown();
        m_viewportTarget.shutdown(m_context, m_uiPass);
        m_uiPass.shutdown();
        destroySwapchainResources();
        m_context.closeDevice();
    }
    m_surface.shutdown(m_context.instance());
    m_context.closeInstance();
    m_rendering = nullptr;
}
