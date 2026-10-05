#pragma once
#include "../assets/Assets.hpp"
#include "../input/InputManager.hpp"
#include "../jobs/FrameTaskGraph.hpp"
#include "../scene/Scene.hpp"
#include "../ui/GameUi.hpp"
#include "../window/Window.hpp"
#include "Rendering.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/DebugDrawPass.hpp"
#include "passes/MeshPass.hpp"
#include "passes/UiPass.hpp"
#include "resources/FrameResources.hpp"
#include "resources/IdReadback.hpp"
#include "resources/ImageReadback.hpp"
#include "resources/UiImageStore.hpp"
#include "resources/ViewportTarget.hpp"
#include "surface/Surface.hpp"
#include "swapchain/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The passes one render view adds to the planned frame graph. Settings that
// only change pass parameters are deliberately absent. Decisions MeshPass
// makes while recording are copied from the view's MeshPassFramePlan, never
// re-derived.
struct RenderGraphViewShape {
    // View slot (RenderViewId::index); names the view's resources.
    uint32_t slot{0};
    uint32_t width{0};
    uint32_t height{0};
    // Renders into an offscreen target the UI samples, else into the swapchain.
    bool offscreen{false};
    AntialiasingMode antialiasing{AntialiasingMode::Off};
    AmbientOcclusionQuality ambientOcclusion{AmbientOcclusionQuality::Off};
    BloomQuality bloom{BloomQuality::Off};
    ExposureMode exposure{ExposureMode::Auto};
    bool environmentBackground{false};
    bool depthPrepass{false};
    bool gpuDriven{false};
    bool screenSpaceReflections{false};
    bool volumetricFog{false};
    bool motionBlur{false};
    bool depthOfField{false};
    // Any colour-grade effect requested (ColorGradePass); a LUT whose file
    // fails to load still counts, so the model may show an idle grade pass.
    bool colorGrade{false};
    // FSR 1: the composite renders into the upscaler's input, then EASU and RCAS.
    bool spatialUpscale{false};
    bool decals{false};
    // Debug lines (RenderRuntime::debugDraw) blend over the finished output.
    bool debugDraw{false};
    // A pick request is served this frame (IdBufferPass); frames without one
    // have no id-buffer pass.
    bool idBuffer{false};

    bool operator==(const RenderGraphViewShape&) const = default;
};

// Every input that changes the planned frame graph's passes: the work all
// views share once per frame, then each recorded view in recording order.
struct RenderGraphShape {
    bool shadows{false};
    bool imageBasedLighting{false};
    bool localLights{false};
    bool localShadows{false};
    bool perPixelReflectionProbes{false};
    bool gpuShadowCulling{false};
    // Irradiance volume (U6)
    bool irradianceVolume{false};
    bool particles{false};
    // views[0] leads the frame (see MeshPass).
    std::vector<RenderGraphViewShape> views;

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
    // Receives renderer and validation-layer messages; must outlive the
    // renderer. Null writes them to stderr.
    Logger* logger{nullptr};
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
    // Frames recorded ahead of the GPU. Each frame slot leases the assets it
    // draws until the slot is reused, so an asset released now is unleased
    // after this many more frames.
    static constexpr uint32_t framesInFlight = 3;

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

    // Offscreen targets of a created render view (slot >= 1), for the
    // RenderRuntime generation they were created for. The default view uses
    // m_viewportTarget. Destroyed only after the device idles, at a
    // beginUiFrame, so UI draw data of the frame that released the view could
    // still sample its texture.
    struct ViewTargets {
        uint32_t generation{0};
        ViewportTarget target;
    };

    FrameStatus prepareSurface(const Window& window);
    bool recreateSwapchain(const Window& window);
    bool prepareViewport();
    // Creates, resizes, and destroys created views' targets to match the
    // RenderRuntime requests, then publishes their textures.
    bool prepareViews();
    void destroyViewTargets(ViewTargets& targets, uint32_t slot);
    FrameStatus acquireImage(const FrameResources& frame, uint32_t& imageIndex);
    bool recordFrame(const FrameResources& frame, uint32_t imageIndex, const Scene& scene, const AssetManager& assets);
    // Fills m_viewRequests with this frame's views in recording order.
    void collectViewRequests(VkImage swapchainImage, VkImageView swapchainView);
    bool presentFrame(uint32_t imageIndex);
    void prepareCapture();
    void finishCapture(const FrameResources& frame);
    void pruneUnusedUiTextures(const AssetManager& assets);
    bool updateRenderGraphPlan();
    // Draws this frame's debug lines into every recorded view and publishes
    // each view's projected debug labels. After MeshPass::record.
    bool recordDebugDraw(VkCommandBuffer commandBuffer);
    // Ends the debug primitives' one-frame lifetime.
    void clearDebugDraw();

    // GPU picking (RenderRuntime::requestPick). A pick recorded into a frame,
    // kept with its readback until that frame completes.
    struct InFlightPick {
        PickTicket ticket;
        PickRect region;
        VkExtent2D viewExtent{};
        std::unique_ptr<IdReadback> readback;
        std::vector<PickedObject> objects;
        glm::mat4 viewProjection{1.0f};
        uint64_t submittedFrame{0};
    };

    // Starts the queued picks of every view in m_viewRequests and points the
    // requests at them.
    void attachPicks();
    // Hands this frame's picks to m_inFlightPicks once it was submitted.
    void submitPicks(uint64_t submittedFrame);
    // Resolves this frame's picks to Failed when the frame was not submitted.
    void failFramePicks(const std::string& error);
    // Publishes the picks whose frame completed; never waits for the device.
    void collectPicks();

    VulkanContext m_context;
    Surface m_surface;
    Swapchain m_swapchain;
    MeshPass m_meshPass;
    UiPass m_uiPass;
    DebugDrawPass m_debugDrawPass;
    // Per-frame scratch of recordDebugDraw.
    std::vector<DebugDrawPassView> m_debugDrawViews;
    UiImageStore m_uiImages;
    ViewportTarget m_viewportTarget;
    // Indexed by view slot; slot 0 is unused (the default view's targets are
    // m_viewportTarget and the swapchain).
    std::vector<std::unique_ptr<ViewTargets>> m_viewTargets;
    // Per-frame scratch, reused to keep allocations off the frame loop. The
    // settings array holds composed settings of views that override them, so
    // requests can point into it.
    std::vector<MeshPassViewRequest> m_viewRequests;
    std::vector<uint32_t> m_renderedViews;
    std::array<RenderSettings, RenderRuntime::maximumViews> m_composedViewSettings;
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
    // This frame's shape, compared against m_renderGraphShape.
    RenderGraphShape m_frameGraphShape;
    RenderGraphStatistics m_renderGraphStatistics;
    // Per entry of m_viewRequests: the barrier that hands an offscreen view's
    // output to the UI pass (empty for a view rendering into the swapchain).
    std::vector<std::optional<RenderGraphBarrier>> m_viewToUiBarriers;
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
    // This frame's picks: MeshPass reads and fills m_framePicks through the
    // view requests; m_framePickStates holds each one's ticket and readback.
    std::vector<MeshPassPick> m_framePicks;
    std::vector<InFlightPick> m_framePickStates;
    std::vector<InFlightPick> m_inFlightPicks;
    // RenderStats::submittedFrame of each frame slot's latest submission.
    std::array<uint64_t, framesInFlight> m_slotSubmittedFrames{};
};
