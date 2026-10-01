#pragma once

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <vector>

using RenderObjectId = uint32_t;

struct UiTextureHandle {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }
};

enum class AntialiasingMode : uint8_t {
    Off,
    Fxaa,
    Taa,
};

enum class AmbientOcclusionQuality : uint8_t {
    Off,
    HalfResolution,
    FullResolution,
};

enum class ReflectionQuality : uint8_t {
    ProbesOnly,
    ScreenSpaceHalfResolution,
    ScreenSpaceFullResolution,
};

enum class ShadowQuality : uint8_t {
    Off,
    Single,
    CascadedBalanced,
    CascadedHigh,
};

enum class LightingQuality : uint8_t {
    Fast,
    PhysicallyBased,
};

enum class BloomQuality : uint8_t {
    Off,
    Downsampled,
};

enum class EnvironmentQuality : uint8_t {
    Hemisphere,
    ImageBasedLow,
    ImageBasedHigh,
};

enum class LocalLightingQuality : uint8_t {
    Disabled,
    Tiled32,
    Tiled64,
};

struct RenderPassBudget {
    double cpuMilliseconds{0.0};
    double gpuMilliseconds{0.0};
};

struct RenderPassBudgets {
    RenderPassBudget shadow;
    RenderPassBudget opaque;
    RenderPassBudget blended;
    RenderPassBudget bloom;
    RenderPassBudget ui;
    RenderPassBudget upload;
    RenderPassBudget temporalResolve;
    RenderPassBudget ambientOcclusion;
    RenderPassBudget reflections;
};

struct DynamicResolutionSettings {
    bool enabled{false};
    float minimumScale{0.67f};
    float maximumScale{1.0f};
    float targetGpuMilliseconds{14.0f};
    float adjustmentRate{0.025f};
    uint32_t quantization{16};
};

struct RenderSettings {
    AntialiasingMode antialiasing{AntialiasingMode::Fxaa};
    ShadowQuality shadows{ShadowQuality::Single};
    LightingQuality lighting{LightingQuality::PhysicallyBased};
    BloomQuality bloom{BloomQuality::Off};
    EnvironmentQuality environment{EnvironmentQuality::Hemisphere};
    LocalLightingQuality localLights{LocalLightingQuality::Tiled32};
    bool reflectionProbes{true};
    bool preferGpuDrivenSubmission{false};
    // Largest projected geometric error, in pixels at internal render
    // resolution, a simplified mesh LOD may show. Zero always draws full detail.
    float lodErrorThresholdPixels{1.0f};
    float cascadeSplitLambda{0.65f};
    float cascadeBlendFraction{0.10f};
    bool visualizeShadowCascades{false};
    DynamicResolutionSettings dynamicResolution;
    AmbientOcclusionQuality ambientOcclusion{AmbientOcclusionQuality::Off};
    ReflectionQuality reflections{ReflectionQuality::ProbesOnly};
    // Zero disables enforcement for that processor/pass pair.
    RenderPassBudgets budgets;
};

struct RenderCapabilities {
    bool descriptorIndexing{false};
    bool multiDrawIndirect{false};
    bool indirectDrawCount{false};
    bool gpuDrivenPath{false};
    bool imageBasedLighting{false};
    bool reflectionProbes{false};
    bool temporalAntialiasing{true};
    bool screenSpaceAmbientOcclusion{true};
    bool screenSpaceReflections{true};
    bool meshShaders{false};
    bool rayTracing{false};
    bool dedicatedTransferQueue{false};
    bool timelineTransfers{false};
    uint32_t maximumLocalLightsPerTile{32};
};

struct RenderStats {
    // CPU fields describe this submission; GPU fields identify a retired one.
    uint64_t submittedFrame{0};
    uint64_t gpuSampleFrame{0};
    bool frameSubmitted{false};
    bool gpuTimingsValid{false};
    bool uploadTimingsValid{false};
    double cpuFrameWaitMilliseconds{0.0};
    double cpuAcquireMilliseconds{0.0};
    double cpuPresentMilliseconds{0.0};
    double cpuPostMilliseconds{0.0};
    uint64_t uploadBytes{0};
    uint32_t uploadBackpressureEvents{0};
    uint32_t renderableCount{0};
    uint32_t visibleCount{0};
    uint32_t culledCount{0};
    uint32_t shadowCasterCount{0};
    uint32_t shadowCascadesRendered{0};
    uint32_t shadowCascadesCached{0};
    uint32_t drawCalls{0};
    uint64_t triangleCount{0};
    double gpuFrameMilliseconds{0.0};
    double gpuShadowMilliseconds{0.0};
    double gpuOpaqueMilliseconds{0.0};
    double gpuBlendedMilliseconds{0.0};
    double gpuSceneMilliseconds{0.0};
    double gpuPostMilliseconds{0.0};
    double gpuBloomMilliseconds{0.0};
    double gpuUiMilliseconds{0.0};
    double gpuUploadMilliseconds{0.0};
    double gpuUploadWaitMilliseconds{0.0};
    double cpuShadowMilliseconds{0.0};
    double cpuOpaqueMilliseconds{0.0};
    double cpuBlendedMilliseconds{0.0};
    double cpuBloomMilliseconds{0.0};
    double cpuUiMilliseconds{0.0};
    double cpuUploadMilliseconds{0.0};
    uint32_t localLightCount{0};
    uint32_t discardedLightAssignments{0};
    double cpuRenderPacketMilliseconds{0.0};
    bool renderPacketsRebuilt{false};
    uint32_t scannedRenderables{0};
    uint32_t updatedObjectRecords{0};
    uint32_t mainThreadAllocations{0};
    uint32_t indirectCommandCount{0};
    uint32_t gpuVisibilityMismatches{0};
    double gpuCullingMilliseconds{0.0};
    uint32_t taskQueueDepth{0};
    uint32_t taskStolenJobs{0};
    double taskWorkerUtilization{0.0};
    double taskMainThreadAssistMilliseconds{0.0};
    double taskGraphLatencyMilliseconds{0.0};
    uint32_t renderGraphBarrierCount{0};
    uint32_t renderGraphQueueTransfers{0};
    uint64_t transientRequestedBytes{0};
    uint64_t transientCommittedBytes{0};
    uint64_t transientAliasedBytes{0};
    uint64_t transientPeakBytes{0};
    uint32_t internalRenderWidth{0};
    uint32_t internalRenderHeight{0};
    float internalResolutionScale{1.0f};
    bool temporalHistoryValid{false};
    bool temporalHistoryInvalidated{false};
    double gpuTemporalMilliseconds{0.0};
    double gpuAmbientOcclusionMilliseconds{0.0};
    double gpuReflectionsMilliseconds{0.0};
    uint64_t temporalHistoryBytes{0};
    uint64_t motionVectorBytes{0};
    uint64_t ambientOcclusionBytes{0};
    uint64_t reflectionBytes{0};
    uint64_t shadowCacheBytes{0};
    uint64_t streamingDecodedBytes{0};
    uint64_t streamingUploadedBytes{0};
    uint64_t streamingDecodeBacklogBytes{0};
    uint64_t streamingUploadBacklogBytes{0};
    uint64_t streamingCpuPeakBytes{0};
    uint64_t streamingGpuPeakBytes{0};
    uint32_t streamingResidencyMisses{0};
    uint32_t streamingHardBudgetFailures{0};
    bool pipelineCacheWarm{false};
    uint32_t pipelineCreationEvents{0};
    double pipelineCreationMilliseconds{0.0};
    uint32_t pipelineFirstUseEvents{0};
    double pipelineFirstUseMilliseconds{0.0};
};

struct RenderView {
    glm::vec3 position{0.0f, 0.0f, 5.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float verticalFovRadians{glm::radians(60.0f)};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
};

struct GridOverlay {
    float planeHeight{0.0f};
    float spacing{1.0f};
    float fadeDistance{40.0f};
};

class RenderRuntime {
  public:
    const RenderSettings& settings() const {
        return m_settings;
    }

    void setSettings(RenderSettings settings) {
        m_settings = settings;
    }

    const RenderCapabilities& capabilities() const {
        return m_capabilities;
    }

    const RenderStats& stats() const {
        return m_stats;
    }

    void requestViewportSize(uint32_t width, uint32_t height) {
        m_viewportRequested = true;
        m_requestedViewportWidth = width;
        m_requestedViewportHeight = height;
    }

    void setViewportInputActive(bool active) {
        m_viewportInputActive = active;
    }

    void setViewOverride(RenderView view) {
        m_viewOverride = view;
    }

    void clearViewOverride() {
        m_viewOverride.reset();
    }

    void setGridOverlay(std::optional<GridOverlay> grid) {
        m_gridOverlay = grid;
    }

    void setOutlinedObjects(std::span<const RenderObjectId> objects) {
        m_outlinedObjects.assign(objects.begin(), objects.end());
        std::ranges::sort(m_outlinedObjects);
        m_outlinedObjects.erase(std::unique(m_outlinedObjects.begin(), m_outlinedObjects.end()),
                                m_outlinedObjects.end());
    }

    void clearOutlinedObjects() {
        m_outlinedObjects.clear();
    }

    UiTextureHandle viewportTexture() const {
        return m_viewportTexture;
    }

    uint32_t viewportWidth() const {
        return m_viewportWidth;
    }

    uint32_t viewportHeight() const {
        return m_viewportHeight;
    }

  private:
    friend class Renderer;
    friend class MeshPass;

    RenderSettings m_settings;
    RenderCapabilities m_capabilities;
    RenderStats m_stats;
    UiTextureHandle m_viewportTexture;
    uint32_t m_requestedViewportWidth{0};
    uint32_t m_requestedViewportHeight{0};
    uint32_t m_viewportWidth{0};
    uint32_t m_viewportHeight{0};
    bool m_viewportRequested{false};
    bool m_viewportInputActive{false};
    std::optional<RenderView> m_viewOverride;
    std::optional<GridOverlay> m_gridOverlay;
    std::vector<RenderObjectId> m_outlinedObjects;
};
