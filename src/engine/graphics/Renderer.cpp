#include "Renderer.hpp"
#include "vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace {
constexpr uint32_t surfaceRetryDelayMs = 16;

void prepareColorAttachment(VkCommandBuffer commandBuffer, VkImage image) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                  .srcAccessMask = 0,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
} // namespace

bool Renderer::init(const Window& window, RenderRuntime& rendering) {
    m_rendering = &rendering;
    if (!initContext(window)) {
        return false;
    }
    rendering.m_capabilities = detectRenderCapabilities(m_context.pDevice());
    rendering.m_capabilities.imageBasedLighting = true;
    rendering.m_capabilities.reflectionProbes = true;
    rendering.m_capabilities.maximumLocalLightsPerTile = 64;
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
    if (!m_meshPass.init(m_context, m_swapchain.format(), m_swapchain.depthFormat(), framesInFlight,
                         m_viewportTarget.extent(), m_viewportTarget.depthView())) {
        return false;
    }
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

bool Renderer::initContext(const Window& window) {
    if (!checkVulkan(volkInitialize(), "volkInitialize")) {
        return false;
    }
    if (!m_context.initInstance()) {
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
    return m_swapchain.initSwapchain(window, m_surface, m_context);
}

void Renderer::destroySwapchainResources() {
    m_swapchain.destroySwapchain(m_context);
}

bool Renderer::drawFrame(const Window& window, Scene& scene, const AssetManager& assets) {
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
    if (!recordFrame(frame, imageIndex, scene, assets)) {
        m_uiPass.cancelFrame();
        return false;
    }
    if (!frame.submit(m_context.device(), m_context.gfxQueue(), m_swapchain.renderComplete(imageIndex))) {
        return false;
    }
    const uint64_t submittedFrame = ++m_submittedFrames;
    frame.markSubmitted(submittedFrame, m_rendering->m_settings.bloom != BloomQuality::Off);
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
        std::cerr << "Cannot query drawable size: " << SDL_GetError() << '\n';
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
    const VkImageLayout oldLayout =
        useViewport ? m_viewportTarget.oldLayout(m_currentFrame) : VK_IMAGE_LAYOUT_UNDEFINED;
    if (!m_meshPass.record(frame.commandBuffer(), colorImage, colorView, m_viewportTarget.depthImage(),
                           m_viewportTarget.depthView(), extent, m_currentFrame, scene, assets, *m_rendering,
                           frame.timestampQueries(), oldLayout)) {
        return false;
    }
    if (useViewport) {
        m_viewportTarget.transitionForSampling(frame.commandBuffer(), m_currentFrame);
        prepareColorAttachment(frame.commandBuffer(), image);
    }
    const auto uiCpuBegin = std::chrono::steady_clock::now();
    if (!m_uiPass.record(frame.commandBuffer(), image, view, m_swapchain.extent(), useViewport)) {
        return false;
    }
    m_rendering->m_stats.cpuUiMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiCpuBegin).count();
    frame.writeTimestamp(5, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    return frame.endRecording();
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
