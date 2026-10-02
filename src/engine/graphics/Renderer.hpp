#pragma once
#include "../assets/Assets.hpp"
#include "../input/InputManager.hpp"
#include "../jobs/FrameTaskGraph.hpp"
#include "../scene/Scene.hpp"
#include "../ui/GameUi.hpp"
#include "../window/Window.hpp"
#include "Rendering.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/MeshPass.hpp"
#include "passes/UiPass.hpp"
#include "resources/FrameResources.hpp"
#include "resources/ImageReadback.hpp"
#include "resources/ViewportTarget.hpp"
#include "surface/Surface.hpp"
#include "swapchain/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include <array>
#include <optional>
#include <string>
#include <unordered_map>

// Every input that changes the planned frame graph's passes. Settings that only
// change pass parameters are deliberately absent.
struct RenderGraphShape {
    uint32_t width{0};
    uint32_t height{0};
    bool viewport{false};
    AntialiasingMode antialiasing{AntialiasingMode::Off};
    AmbientOcclusionQuality ambientOcclusion{AmbientOcclusionQuality::Off};
    ReflectionQuality reflections{ReflectionQuality::ProbesOnly};
    BloomQuality bloom{BloomQuality::Off};
    ExposureMode exposure{ExposureMode::Auto};
    bool shadows{false};
    bool imageBasedLighting{false};
    bool environmentBackground{false};
    bool localLights{false};
    bool localShadows{false};
    bool perPixelReflectionProbes{false};
    bool depthPrepass{false};
    bool gpuDriven{false};
    bool volumetricFog{false};
    // Irradiance volume (U6)
    bool irradianceVolume{false};
    bool decals{false};

    bool operator==(const RenderGraphShape&) const = default;
};

// Diagnostic options fixed for the renderer's lifetime.
struct RendererOptions {
    // Creates the swapchain with TRANSFER_SRC usage so requestFrameCapture()
    // can copy presented images.
    bool frameCapture{false};
    // See PostProcessPass::setFixedFrameSeconds.
    std::optional<float> fixedFrameSeconds;
    // Validation-layer policy; strict runs enable synchronization validation.
    GraphicsValidationOptions validation;
};

// Outcome of a requested frame capture: an image or an error. Neither is set
// when the frame was skipped before recording (minimized window or an out of
// date swapchain).
struct FrameCaptureResult {
    std::optional<RgbaImage> image;
    std::string error;
};

class Renderer {
  public:
    bool init(const Window& window, RenderRuntime& rendering, FrameTaskExecutor& frameTasks,
              const RendererOptions& options = {});
    bool drawFrame(const Window& window, Scene& scene, const AssetManager& assets);

    // Copies the swapchain image the next drawFrame presents, after every pass
    // including UI, and waits for it before presenting. Requires
    // RendererOptions::frameCapture.
    void requestFrameCapture() {
        m_captureRequested = true;
    }

    // Returns and clears the result of the capture requested before the latest
    // drawFrame.
    FrameCaptureResult takeFrameCapture();
    void processUiEvent(const SDL_Event& event);
    void beginUiFrame(const AssetManager& assets);
    void drawGameUi(const GameUiContext& gameUi, const AssetManager& assets);
    InputCapture uiInputCapture() const;

    const char* presentModeName() const {
        return m_swapchain.presentModeName();
    }

    // Validation-layer message counts for this renderer; valid after shutdown.
    GraphicsValidationReport validationReport() const {
        return m_context.validationReport();
    }

    const std::string& gpuName() const {
        return m_gpuName;
    }

    uint64_t gpuAllocationBytes() const;

    // Compressed formats the device samples; asset import uses this to pick
    // cooked textures over RGBA8 source images.
    TextureFormatCapabilities textureFormatCapabilities() const {
        const bool bc = m_context.blockCompressionSupported();
        return {.bc7 = bc, .bc5 = bc};
    }

    VkExtent2D outputExtent() const {
        return m_swapchain.extent();
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

    bool initContext(const Window& window, const GraphicsValidationOptions& validation);
    bool createSwapchainResources(const Window& window);
    void destroySwapchainResources();
    FrameStatus prepareSurface(const Window& window);
    bool recreateSwapchain(const Window& window);
    bool prepareViewport();
    FrameStatus acquireImage(const FrameResources& frame, uint32_t& imageIndex);
    bool recordFrame(const FrameResources& frame, uint32_t imageIndex, const Scene& scene, const AssetManager& assets);
    bool presentFrame(uint32_t imageIndex);
    void prepareCapture();
    void finishCapture(const FrameResources& frame);
    void pruneUnusedUiTextures(const AssetManager& assets);
    bool updateRenderGraphPlan(VkExtent2D extent, bool useViewport);

    VulkanContext m_context;
    Surface m_surface;
    Swapchain m_swapchain;
    MeshPass m_meshPass;
    UiPass m_uiPass;
    ViewportTarget m_viewportTarget;
    static constexpr uint32_t framesInFlight = 3;
    std::array<FrameResources, framesInFlight> m_frames;
    std::array<AssetLeaseSet, framesInFlight> m_frameAssets;
    AssetLeaseSet m_pendingUiAssets;
    uint32_t m_currentFrame{0};
    uint64_t m_submittedFrames{0};
    bool m_resizeRequested{false};
    bool m_viewportFailed{false};
    RenderRuntime* m_rendering{nullptr};
    FrameTaskExecutor* m_frameTasks{nullptr};
    std::string m_gpuName;
    std::unordered_map<uint64_t, uint64_t> m_gameUiTextures;
    // Empty until the first plan, so the first frame always plans.
    std::optional<RenderGraphShape> m_renderGraphShape;
    RenderGraphStatistics m_renderGraphStatistics;
    std::optional<RenderGraphBarrier> m_viewportToUiBarrier;
    std::optional<RenderGraphBarrier> m_swapchainToUiBarrier;
    double m_pipelineCreationMilliseconds{0.0};
    double m_pipelineFirstUseMilliseconds{0.0};
    uint32_t m_prewarmedPipelineCount{0};
    bool m_pipelineCacheWarm{false};
    bool m_pipelineFirstUsePending{true};
    RendererOptions m_options;
    ImageReadback m_readback;
    FrameCaptureResult m_capture;
    bool m_captureRequested{false};
    // Set between a successful prepareCapture and finishCapture; recordFrame
    // then records the copy.
    bool m_capturePrepared{false};
};
