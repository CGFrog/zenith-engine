#pragma once
#include "../assets/Assets.hpp"
#include "../input/InputManager.hpp"
#include "../scene/Scene.hpp"
#include "../ui/GameUi.hpp"
#include "../window/Window.hpp"
#include "Rendering.hpp"
#include "passes/MeshPass.hpp"
#include "passes/UiPass.hpp"
#include "resources/FrameResources.hpp"
#include "resources/ViewportTarget.hpp"
#include "surface/Surface.hpp"
#include "swapchain/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include <array>
#include <unordered_map>

class Renderer {
  public:
    bool init(const Window& window, RenderRuntime& rendering);
    bool drawFrame(const Window& window, Scene& scene, const AssetManager& assets);
    void processUiEvent(const SDL_Event& event);
    void beginUiFrame(const AssetManager& assets);
    void drawGameUi(const GameUiContext& gameUi, const AssetManager& assets);
    InputCapture uiInputCapture() const;

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
    bool prepareViewport();
    FrameStatus acquireImage(const FrameResources& frame, uint32_t& imageIndex);
    bool recordFrame(const FrameResources& frame, uint32_t imageIndex, const Scene& scene, const AssetManager& assets);
    bool presentFrame(uint32_t imageIndex);

    VulkanContext m_context;
    Surface m_surface;
    Swapchain m_swapchain;
    MeshPass m_meshPass;
    UiPass m_uiPass;
    ViewportTarget m_viewportTarget;
    static constexpr uint32_t framesInFlight = 3;
    std::array<FrameResources, framesInFlight> m_frames;
    uint32_t m_currentFrame{0};
    uint32_t m_timingSampleCounter{0};
    bool m_resizeRequested{false};
    bool m_viewportFailed{false};
    RenderRuntime* m_rendering{nullptr};
    std::unordered_map<uint64_t, uint64_t> m_gameUiTextures;
};
