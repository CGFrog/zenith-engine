#include "Renderer.hpp"
#include "ColorGrading.hpp"
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

// Settings-driven choices come from `settings`; every choice MeshPass makes
// while recording comes from the frame's `plan`, never re-derived here.
RenderGraphShape renderGraphShape(VkExtent2D extent, bool viewport, const RenderSettings& settings,
                                  const RenderCapabilities& capabilities, const MeshPassFramePlan& plan) {
    const bool imageBasedLighting = settings.environment != EnvironmentQuality::Hemisphere;
    return {.width = extent.width,
            .height = extent.height,
            .viewport = viewport,
            .antialiasing = settings.antialiasing,
            .ambientOcclusion = plan.ambientOcclusion,
            .bloom = settings.bloom,
            .exposure = settings.exposure.mode,
            .shadows = settings.shadows != ShadowQuality::Off,
            .imageBasedLighting = imageBasedLighting,
            .environmentBackground = imageBasedLighting && settings.environmentBackground,
            .localLights = settings.localLights != LocalLightingQuality::Disabled,
            .localShadows = settings.localLights != LocalLightingQuality::Disabled &&
                            settings.localShadows != LocalShadowQuality::Off,
            // The pass also skips frames whose scene has no resident probe.
            .perPixelReflectionProbes = plan.perPixelReflectionProbes,
            .depthPrepass = plan.depthPrepass,
            .gpuDriven = plan.gpuDrivenSubmission,
            .gpuShadowCulling = plan.gpuShadowCulling,
            .screenSpaceReflections = plan.screenSpaceReflections,
            .volumetricFog = settings.volumetricFog != VolumetricFogQuality::Off,
            .irradianceVolume = settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume,
            .motionBlur = settings.motionBlur != MotionBlurQuality::Off,
            .depthOfField = settings.depthOfField.quality != DepthOfFieldQuality::Off,
            .colorGrade = color_grading::lutRequested(settings.colorLut) ||
                          color_grading::vignetteActive(settings.vignette) ||
                          color_grading::filmGrainActive(settings.filmGrain),
            // Mirrors the spatialUpscale decision in PostProcessPass::record.
            .spatialUpscale = settings.upscaler.mode == UpscalerMode::Fsr1 && capabilities.spatialUpscaling,
            .decals = settings.decals != DecalQuality::Off,
            .particles = settings.particles != ParticleQuality::Off};
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
    if (!createSwapchainResources(window)) {
        return false;
    }
    if (!m_uiPass.init(m_context, window, m_swapchain.format(), m_swapchain.imageCount())) {
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
    GpuFrameTimings gpuTimings;
    if (!frame.collectTimings(m_context.device(), gpuTimings)) {
        m_uiPass.cancelFrame();
        return false;
    }
    pruneUnusedUiTextures(assets);
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
        return false;
    }
    const uint64_t submittedFrame = ++m_submittedFrames;
    frame.markSubmitted(submittedFrame, m_rendering->m_settings.bloom != BloomQuality::Off,
                        m_rendering->m_settings.antialiasing == AntialiasingMode::Taa,
                        m_rendering->m_settings.ambientOcclusion != AmbientOcclusionQuality::Off,
                        m_rendering->m_settings.reflections != ReflectionQuality::ProbesOnly,
                        m_rendering->m_stats.uploadBytes != 0);
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
    destroySwapchainResources();
    if (!createSwapchainResources(window)) {
        return false;
    }
    if (!m_rendering->m_viewportRequested) {
        if (!m_viewportTarget.resize(m_context, m_uiPass, m_swapchain.extent()) ||
            !m_meshPass.resize(m_context, m_viewportTarget.extent(), m_viewportTarget.depthView())) {
            return false;
        }
    }
    m_resizeRequested = false;
    return true;
}

bool Renderer::prepareViewport() {
    if (!m_rendering->m_viewportRequested) {
        return true;
    }
    const VkExtent2D requested{std::clamp(m_rendering->m_requestedViewportWidth, 16u, 8192u),
                               std::clamp(m_rendering->m_requestedViewportHeight, 16u, 8192u)};
    if (requested.width == m_viewportTarget.extent().width && requested.height == m_viewportTarget.extent().height) {
        return true;
    }
    if (!checkVulkan(vkDeviceWaitIdle(m_context.device()), "Wait before viewport resize")) {
        return false;
    }
    return m_viewportTarget.resize(m_context, m_uiPass, requested) &&
           m_meshPass.resize(m_context, requested, m_viewportTarget.depthView());
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
    const VkImage image = m_swapchain.swapchainImages()[imageIndex];
    const VkImageView view = m_swapchain.swapchainImageViews()[imageIndex];
    const bool useViewport = m_rendering->m_viewportRequested;
    const VkImage colorImage = useViewport ? m_viewportTarget.image(m_currentFrame) : image;
    const VkImageView colorView = useViewport ? m_viewportTarget.view(m_currentFrame) : view;
    const VkExtent2D extent = useViewport ? m_viewportTarget.extent() : m_swapchain.extent();
    // One plan drives both the descriptive graph and the recorded passes.
    const MeshPassFramePlan meshPassPlan = m_meshPass.planFrame(m_rendering->settings(), m_rendering->capabilities());
    if (!updateRenderGraphPlan(extent, useViewport, meshPassPlan)) {
        return false;
    }
    const VkImageLayout oldLayout =
        useViewport ? m_viewportTarget.oldLayout(m_currentFrame) : VK_IMAGE_LAYOUT_UNDEFINED;
    if (!m_meshPass.record(frame.commandBuffer(), colorImage, colorView, m_viewportTarget.depthImage(),
                           m_viewportTarget.depthView(), extent, m_currentFrame, scene, assets, *m_rendering,
                           meshPassPlan, *m_frameTasks, frame.timestampQueries(), oldLayout)) {
        return false;
    }
    RenderStats& graphStats = m_rendering->m_stats;
    graphStats.renderGraphBarrierCount = m_renderGraphStatistics.barrierCount;
    graphStats.renderGraphQueueTransfers = m_renderGraphStatistics.queueTransferCount;
    graphStats.transientRequestedBytes = m_renderGraphStatistics.requestedTransientBytes;
    graphStats.transientCommittedBytes = m_renderGraphStatistics.committedTransientBytes;
    graphStats.transientAliasedBytes = m_renderGraphStatistics.aliasedTransientBytes;
    graphStats.transientPeakBytes = m_renderGraphStatistics.peakTransientBytes;
    if (useViewport) {
        if (!m_viewportToUiBarrier || !m_swapchainToUiBarrier) {
            return false;
        }
        applyGraphBarrier(frame.commandBuffer(), colorImage, *m_viewportToUiBarrier);
        m_viewportTarget.markWritten(m_currentFrame);
        applyGraphBarrier(frame.commandBuffer(), image, *m_swapchainToUiBarrier);
    } else {
        if (!m_swapchainToUiBarrier) {
            return false;
        }
        applyGraphBarrier(frame.commandBuffer(), image, *m_swapchainToUiBarrier);
    }
    const auto uiCpuBegin = std::chrono::steady_clock::now();
    if (!m_uiPass.record(frame.commandBuffer(), image, view, m_swapchain.extent(), useViewport)) {
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

bool Renderer::updateRenderGraphPlan(VkExtent2D extent, bool useViewport, const MeshPassFramePlan& meshPassPlan) {
    const RenderGraphShape shape =
        renderGraphShape(extent, useViewport, m_rendering->settings(), m_rendering->capabilities(), meshPassPlan);
    if (shape != m_renderGraphShape) {
        RenderGraph graph;
        const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
        const auto image = [&](uint32_t format, uint64_t usage, uint64_t bytes) {
            return RenderGraphImageDescription{.format = format,
                                               .width = extent.width,
                                               .height = extent.height,
                                               .usage = usage,
                                               .requestedBytes = bytes,
                                               .alignment = 256};
        };
        const auto swapchain =
            graph.importImage("swapchain", image(m_swapchain.format(), 1, 0), VK_IMAGE_LAYOUT_UNDEFINED);
        const auto output = useViewport ? graph.importImage("editor viewport", image(m_swapchain.format(), 3, 0),
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                        : swapchain;
        const auto depth =
            graph.importImage("scene depth", image(m_swapchain.depthFormat(), 5, 0), VK_IMAGE_LAYOUT_UNDEFINED);
        const auto shadow = graph.importImage("shadow atlas", image(VK_FORMAT_D32_SFLOAT, 5, 0),
                                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        const auto history = graph.importImage("temporal history", image(TemporalResolvePass::historyFormat, 5, 0),
                                               TemporalResolvePass::resolvedLayout);
        const auto probeCache = graph.importImage("reflection probe cache", image(PostProcessPass::sceneFormat, 5, 0),
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const RenderGraphQueue uploadQueue = m_rendering->capabilities().dedicatedTransferQueue
                                                 ? RenderGraphQueue::Transfer
                                                 : RenderGraphQueue::Graphics;
        const auto meshAssets =
            graph.importBuffer("mesh asset buffers", {.size = 0, .usage = 1, .alignment = 256}, uploadQueue);
        const auto textureAssets = graph.importImage("texture asset images", image(VK_FORMAT_R8G8B8A8_UNORM, 5, 0),
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, uploadQueue);
        // Persistent renderer resources the scene passes exchange. Their sizes
        // are owned by the passes, so the model records zero bytes for them.
        const auto environmentMaps = graph.importImage("environment maps", image(PostProcessPass::sceneFormat, 5, 0),
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const auto lightClusters = graph.importBuffer("light clusters", {.size = 0, .usage = 1, .alignment = 256});
        // Persistent: cached tiles keep their depth across frames.
        const auto localShadowAtlas = graph.importImage("local shadow atlas", image(m_swapchain.depthFormat(), 5, 0),
                                                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        const auto probeClusters =
            graph.importBuffer("reflection probe clusters", {.size = 0, .usage = 1, .alignment = 256});
        const auto decalClusters = graph.importBuffer("decal clusters", {.size = 0, .usage = 1, .alignment = 256});
        const auto gpuDrivenDraws = graph.importBuffer("GPU-driven draws", {.size = 0, .usage = 1, .alignment = 256});
        const auto normals =
            graph.importImage("scene normals", image(PostProcessPass::normalFormat, 5, 0), VK_IMAGE_LAYOUT_UNDEFINED);
        // Written by the prepass only on frames that trace screen-space reflections.
        const auto specular = graph.importImage("scene specular", image(PostProcessPass::specularFormat, 5, 0),
                                                VK_IMAGE_LAYOUT_UNDEFINED);
        const auto depthPyramid =
            graph.importImage("depth pyramid", image(DepthPyramid::format, 5, 0), VK_IMAGE_LAYOUT_GENERAL);
        const auto hdr = graph.createTransientImage(
            "HDR scene", image(PostProcessPass::sceneFormat, 7, pixels * sizeof(uint16_t) * 4));
        const auto motion = graph.createTransientImage(
            "motion and reactive mask", image(PostProcessPass::motionFormat, 5, pixels * sizeof(uint16_t) * 4));
        // Persistent per frame slot; AmbientOcclusionPass keeps it in GENERAL.
        const auto ambientOcclusion =
            graph.importImage("ambient occlusion", image(AmbientOcclusionPass::occlusionFormat, 5, 0),
                              AmbientOcclusionPass::occlusionLayout);
        // Persistent and shared by consecutive frames, like the temporal history.
        const auto reflections =
            graph.importImage("screen-space reflection target", image(ScreenSpaceReflectionPass::targetFormat, 5, 0),
                              VK_IMAGE_LAYOUT_GENERAL);
        // Persistent froxel volumes; VolumetricFogPass keeps them in GENERAL.
        const auto fogVolume = graph.importImage("volumetric fog volume", image(VolumetricFogPass::volumeFormat, 5, 0),
                                                 VK_IMAGE_LAYOUT_GENERAL);
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
        if (shape.gpuDriven) {
            // The early phase emits last frame's visible set; it binds the
            // pyramid statically but only the late phase samples it.
            const RenderGraphAccess earlyCullAccesses[]{
                {gpuDrivenDraws, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("GPU culling (early)", RenderGraphQueue::Graphics, earlyCullAccesses);
        }
        if (shape.localLights) {
            const RenderGraphAccess clusterAccesses[]{
                {lightClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("light cluster assignment", RenderGraphQueue::Graphics, clusterAccesses);
        }
        if (shape.perPixelReflectionProbes) {
            const RenderGraphAccess probeClusterAccesses[]{
                {probeClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("reflection probe cluster assignment", RenderGraphQueue::Graphics, probeClusterAccesses);
        }
        if (shape.decals) {
            // Skipped on frames whose scene has no decal.
            const RenderGraphAccess decalClusterAccesses[]{
                {decalClusters, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("decal cluster assignment", RenderGraphQueue::Graphics, decalClusterAccesses);
        }
        if (shape.volumetricFog) {
            // Injects the medium lit by the shadowed sun and clustered lights,
            // reprojects last frame's volume, and integrates along depth.
            std::vector<RenderGraphAccess> fogAccesses{
                {fogVolume, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | computeReadWrite, VK_IMAGE_LAYOUT_GENERAL}};
            if (shape.shadows) {
                fogAccesses.push_back({shadow, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
            }
            if (shape.localLights) {
                fogAccesses.push_back({lightClusters, RenderGraphAccessMode::Read,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (shape.irradianceVolume) {
                // Ambient in-scattering inside the volume comes from its probes.
                fogAccesses.push_back({irradianceProbes, RenderGraphAccessMode::Read,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED});
            }
            graph.addPass("volumetric fog", RenderGraphQueue::Graphics, fogAccesses);
        }
        // Persistent particle pool, sort keys, and indirect draw arguments owned
        // by ParticlePass; the passes record work only when the scene has emitters.
        const auto particlePool = graph.importBuffer("particle pool", {.size = 0, .usage = 1, .alignment = 256});
        if (shape.particles) {
            // Emits, simulates, lights (sun shadows, clustered lights, ambient
            // probes), and sorts the particles.
            std::vector<RenderGraphAccess> particleAccesses{
                {particlePool, RenderGraphAccessMode::ReadWrite,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT | computeReadWrite, VK_IMAGE_LAYOUT_UNDEFINED}};
            if (shape.shadows) {
                particleAccesses.push_back({shadow, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
            }
            if (shape.localLights) {
                particleAccesses.push_back({lightClusters, RenderGraphAccessMode::Read,
                                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED});
            }
            if (shape.irradianceVolume) {
                particleAccesses.push_back({irradianceProbes, RenderGraphAccessMode::Read,
                                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED});
            }
            graph.addPass("particle simulation", RenderGraphQueue::Graphics, particleAccesses);
        }
        const auto addDepthPyramidBuild = [&](const char* name) {
            const RenderGraphAccess pyramidAccesses[]{
                {depth, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {depthPyramid, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
            graph.addPass(name, RenderGraphQueue::Graphics, pyramidAccesses);
        };
        const auto addLateCull = [&] {
            const RenderGraphAccess lateCullAccesses[]{
                {depthPyramid, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL},
                {gpuDrivenDraws, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 computeReadWrite, VK_IMAGE_LAYOUT_UNDEFINED}};
            graph.addPass("GPU culling (late)", RenderGraphQueue::Graphics, lateCullAccesses);
        };
        const RenderGraphAccess indirectDrawRead{gpuDrivenDraws, RenderGraphAccessMode::Read,
                                                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        if (shape.depthPrepass) {
            std::vector<RenderGraphAccess> prepassAccesses{
                {depth, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL},
                {normals, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                {meshAssets, RenderGraphAccessMode::Read, geometryStages, geometryAccess, VK_IMAGE_LAYOUT_UNDEFINED}};
            if (shape.gpuDriven) {
                prepassAccesses.push_back(indirectDrawRead);
            }
            if (shape.screenSpaceReflections) {
                prepassAccesses.push_back(
                    {specular, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            }
            graph.addPass("depth and normal prepass", RenderGraphQueue::Graphics, prepassAccesses);
            if (shape.gpuDriven) {
                // Two-phase occlusion culling runs inside the prepass.
                addDepthPyramidBuild("occlusion depth pyramid");
                addLateCull();
                graph.addPass("depth and normal prepass (late phase)", RenderGraphQueue::Graphics, prepassAccesses);
            }
            // Screen-space effects read this pyramid of the complete opaque depth.
            addDepthPyramidBuild("depth pyramid");
        }
        // GTAO runs between the depth prepass and shading. It always records:
        // without the prepass or with AO off it writes unoccluded values.
        std::vector<RenderGraphAccess> ambientOcclusionAccesses{
            {ambientOcclusion, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, AmbientOcclusionPass::occlusionLayout}};
        if (shape.ambientOcclusion != AmbientOcclusionQuality::Off) {
            ambientOcclusionAccesses.push_back(
                {normals, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            ambientOcclusionAccesses.push_back({depthPyramid, RenderGraphAccessMode::Read,
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL});
        }
        graph.addPass("ambient occlusion", RenderGraphQueue::Graphics, ambientOcclusionAccesses);
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
        opaqueAccesses.push_back(shape.depthPrepass ? RenderGraphAccess{depth, RenderGraphAccessMode::Read,
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
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL});
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
        if (shape.decals) {
            opaqueAccesses.push_back({decalClusters, RenderGraphAccessMode::Read,
                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED});
        }
        if (shape.gpuDriven) {
            opaqueAccesses.push_back(indirectDrawRead);
        }
        if (shape.volumetricFog) {
            // Opaque, blended, and sky shading composite the integrated fog.
            opaqueAccesses.push_back({fogVolume, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL});
        }
        if (shape.irradianceVolume) {
            opaqueAccesses.push_back({irradianceProbes, RenderGraphAccessMode::Read,
                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED});
        }
        if (shape.gpuDriven && !shape.depthPrepass) {
            // Without a prepass two-phase culling splits the shading pass.
            graph.addPass("opaque scene (early phase)", RenderGraphQueue::Graphics, opaqueAccesses);
            addDepthPyramidBuild("occlusion depth pyramid");
            addLateCull();
        }
        graph.addPass("opaque and blended scene", RenderGraphQueue::Graphics, opaqueAccesses);
        if (shape.screenSpaceReflections) {
            // Traces the prepass
            // normals and depth pyramid into the reflection target and blends it
            // into the scene in place. It runs inside the scene pass, after
            // opaque surfaces and the background and before blended ones, which
            // this graph models as one pass.
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
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
            graph.addPass("screen-space reflections", RenderGraphQueue::Graphics, reflectionAccesses);
        }
        if (shape.particles) {
            // Inside the scene pass after blended surfaces, in its own rendering
            // scope: premultiplied quads over the HDR target, the reactive mask
            // in the motion target, and the opaque depth read-only (tested and
            // sampled for soft particles).
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
            if (shape.volumetricFog) {
                particleDrawAccesses.push_back({fogVolume, RenderGraphAccessMode::Read,
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL});
            }
            graph.addPass("particles", RenderGraphQueue::Graphics, particleDrawAccesses);
        }
        const bool temporalResolve = shape.antialiasing == AntialiasingMode::Taa;
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
            graph.addPass("temporal resolve", RenderGraphQueue::Graphics, resolveAccesses);
        }
        // The scene the later passes read: the resolved history with TAA, the
        // HDR target otherwise, or the depth of field output when it runs.
        RenderGraphAccess sceneComputeRead =
            temporalResolve
                ? RenderGraphAccess{history, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, TemporalResolvePass::resolvedLayout}
                : RenderGraphAccess{hdr, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        // Persistent and shared by consecutive frames; DepthOfFieldPass keeps
        // its gather targets and output in GENERAL.
        const auto focusedScene = graph.importImage(
            "depth of field targets", image(DepthOfFieldPass::imageFormat, 5, 0), DepthOfFieldPass::outputLayout);
        if (shape.depthOfField) {
            const RenderGraphAccess depthOfFieldAccesses[]{
                sceneComputeRead,
                {motion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {focusedScene, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, DepthOfFieldPass::outputLayout}};
            graph.addPass("depth of field", RenderGraphQueue::Graphics, depthOfFieldAccesses);
            sceneComputeRead = {focusedScene, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, DepthOfFieldPass::outputLayout};
        }
        // Persistent blurred scene and velocity tiles; MotionBlurPass keeps
        // them in GENERAL and synchronizes them across frames. Motion blur
        // reads the focused scene when depth of field runs.
        const auto motionBlurred = graph.importImage("motion blur target", image(MotionBlurPass::colorFormat, 5, 0),
                                                     MotionBlurPass::blurredLayout);
        if (shape.motionBlur) {
            const RenderGraphAccess motionBlurAccesses[]{
                sceneComputeRead,
                {motion, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {motionBlurred, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, MotionBlurPass::blurredLayout}};
            graph.addPass("motion blur", RenderGraphQueue::Graphics, motionBlurAccesses);
            sceneComputeRead = {motionBlurred, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, MotionBlurPass::blurredLayout};
        }
        if (shape.exposure == ExposureMode::Auto) {
            graph.addPass("luminance histogram", RenderGraphQueue::Graphics, {&sceneComputeRead, 1});
        }
        if (shape.bloom != BloomQuality::Off) {
            graph.addPass("bloom chain", RenderGraphQueue::Graphics, {&sceneComputeRead, 1});
        }
        // With a grade effect, tone mapping writes the grade's persistent
        // display target and the grade writes the output.
        const auto displayTarget =
            graph.importImage("colour-grade display target", image(ColorGradePass::displayFormat, 5, 0),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // With FSR 1 the composite renders at render resolution into the
        // upscaler's persistent input target instead, and RCAS writes what
        // the composite would have.
        const auto upscaleInput = shape.spatialUpscale
                                      ? graph.importImage("FSR input", image(m_swapchain.format(), 5, 0),
                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                      : output;
        const auto gradeInput = shape.colorGrade ? displayTarget : output;
        const auto toneMapTarget = shape.spatialUpscale ? upscaleInput : gradeInput;
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
        if (shape.depthOfField) {
            postAccesses.push_back({focusedScene, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, DepthOfFieldPass::outputLayout});
        }
        graph.addPass("post-process and tone map", RenderGraphQueue::Graphics, postAccesses);
        if (shape.spatialUpscale) {
            // Persistent output-resolution EASU result; SpatialUpscalePass
            // writes it in GENERAL and RCAS samples it read-only.
            const auto upscaled =
                graph.importImage("FSR upscaled scene", image(SpatialUpscalePass::upscaledFormat, 5, 0),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            const RenderGraphAccess easuAccesses[]{
                {upscaleInput, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {upscaled, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
            graph.addPass("FSR EASU upsample", RenderGraphQueue::Graphics, easuAccesses);
            const RenderGraphAccess rcasAccesses[]{
                {upscaled, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {gradeInput, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            graph.addPass("FSR RCAS sharpen", RenderGraphQueue::Graphics, rcasAccesses);
        }
        if (shape.colorGrade) {
            const RenderGraphAccess gradeAccesses[]{
                {displayTarget, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {output, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            graph.addPass("colour grade", RenderGraphQueue::Graphics, gradeAccesses);
        }
        RenderGraphPassHandle uiPass;
        if (useViewport) {
            const RenderGraphAccess uiAccesses[]{
                {output, RenderGraphAccessMode::Read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {swapchain, RenderGraphAccessMode::Write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            uiPass = graph.addPass("editor UI", RenderGraphQueue::Graphics, uiAccesses);
        } else {
            const RenderGraphAccess uiAccesses[]{
                {swapchain, RenderGraphAccessMode::ReadWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            uiPass = graph.addPass("runtime UI", RenderGraphQueue::Graphics, uiAccesses);
        }
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
        m_viewportToUiBarrier.reset();
        m_swapchainToUiBarrier.reset();
        for (const RenderGraphBarrier& barrier : compiled.graph.barriers) {
            if (barrier.destinationPass != uiPass) {
                continue;
            }
            if (useViewport && barrier.resource == output) {
                m_viewportToUiBarrier = barrier;
            }
            if (barrier.resource == swapchain) {
                m_swapchainToUiBarrier = barrier;
                // A first use of the freshly acquired image must start at the
                // stage the frame waits on the acquire semaphore, or its layout
                // transition races the presentation engine's read.
                if (static_cast<VkImageLayout>(barrier.oldLayout) == VK_IMAGE_LAYOUT_UNDEFINED) {
                    m_swapchainToUiBarrier->sourceStages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
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

void Renderer::processUiEvent(const SDL_Event& event) {
    m_uiPass.processEvent(event);
}

void Renderer::beginUiFrame(const AssetManager& assets) {
    if (!prepareViewport()) {
        m_viewportFailed = true;
    }
    if (m_rendering->m_viewportRequested) {
        m_rendering->m_viewportTexture = {m_viewportTarget.texture(m_currentFrame)};
        m_rendering->m_viewportWidth = m_viewportTarget.extent().width;
        m_rendering->m_viewportHeight = m_viewportTarget.extent().height;
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
    const bool viewportInput = m_rendering != nullptr && m_rendering->m_viewportInputActive;
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
        m_readback.release();
        for (FrameResources& frame : m_frames) {
            frame.shutdown(device);
        }
        m_meshPass.shutdown(m_context);
        m_viewportTarget.shutdown(m_context, m_uiPass);
        m_uiPass.shutdown();
        destroySwapchainResources();
        m_context.closeDevice();
    }
    m_surface.shutdown(m_context.instance());
    m_context.closeInstance();
    m_rendering = nullptr;
}
