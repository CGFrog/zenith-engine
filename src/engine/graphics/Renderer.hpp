#pragma once
#include "../assets/Assets.hpp"
#include "../scene/Scene.hpp"
#include "../window/Window.hpp"
#include "passes/MeshPass.hpp"
#include "resources/FrameResources.hpp"
#include "surface/Surface.hpp"
#include "swapchain/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include <array>

class Renderer {
  public:
    bool init(const Window& window);
    bool drawFrame(const Window& window, Scene& scene, const AssetManager& assets);

    const char* presentModeName() const {
        return m_swapchain.presentModeName();
    }

    void requestResize() {
        m_resizeRequested = true;
    }

    void shutdown();

  private:
    enum class FrameStatus {
        Ready,
        Skipped,
        Failed
    };

    bool initContext(const Window& window);
    bool createSwapchainResources(const Window& window);
    void destroySwapchainResources();
    FrameStatus prepareSurface(const Window& window);
    bool recreateSwapchain(const Window& window);
    FrameStatus acquireImage(const FrameResources& frame, uint32_t& imageIndex);
    bool recordFrame(const FrameResources& frame, uint32_t imageIndex, const Scene& scene, const AssetManager& assets);
    bool presentFrame(uint32_t imageIndex);

    VulkanContext m_context;
    Surface m_surface;
    Swapchain m_swapchain;
    MeshPass m_meshPass;
    static constexpr uint32_t framesInFlight = 3;
    std::array<FrameResources, framesInFlight> m_frames;
    uint32_t m_currentFrame{0};
    bool m_resizeRequested{false};
};
