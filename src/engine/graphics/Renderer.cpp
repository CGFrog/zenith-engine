#include "Renderer.hpp"
#include "vulkan/VulkanCheck.hpp"

namespace {
constexpr uint32_t surfaceRetryDelayMs = 16;
}

bool Renderer::init(const Window& window) {
    if (!initContext(window)) {
        return false;
    }
    if (!createSwapchainResources(window)) {
        return false;
    }
    if (!m_meshPass.init(m_context, m_swapchain.format(), m_swapchain.depthFormat(), framesInFlight)) {
        return false;
    }
    m_currentFrame = 0;
    for (FrameResources& frame : m_frames) {
        if (!frame.init(m_context.device(), m_context.gfxQueueIndex())) {
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
    scene.updateWorldTransforms();
    const FrameStatus surfaceStatus = prepareSurface(window);
    if (surfaceStatus != FrameStatus::Ready) {
        return surfaceStatus == FrameStatus::Skipped;
    }
    const FrameResources& frame = m_frames[m_currentFrame];
    if (!frame.waitUntilAvailable(m_context.device())) {
        return false;
    }

    uint32_t imageIndex = 0;
    const FrameStatus acquireStatus = acquireImage(frame, imageIndex);
    if (acquireStatus != FrameStatus::Ready) {
        return acquireStatus == FrameStatus::Skipped;
    }
    if (!recordFrame(frame, imageIndex, scene, assets)) {
        return false;
    }
    if (!frame.submit(m_context.device(), m_context.gfxQueue(), m_swapchain.renderComplete(imageIndex))) {
        return false;
    }
    const bool presented = presentFrame(imageIndex);
    m_currentFrame = (m_currentFrame + 1) % framesInFlight;
    return presented;
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
    m_resizeRequested = false;
    return true;
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
    if (!m_meshPass.record(frame.commandBuffer(), image, view, m_swapchain.depthImage(), m_swapchain.depthImageView(),
                           m_swapchain.extent(), m_currentFrame, scene, assets)) {
        return false;
    }
    return frame.endRecording();
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
        for (FrameResources& frame : m_frames) {
            frame.shutdown(device);
        }
        m_meshPass.shutdown(m_context);
        destroySwapchainResources();
        m_context.closeDevice();
    }
    m_surface.shutdown(m_context.instance());
    m_context.closeInstance();
}
