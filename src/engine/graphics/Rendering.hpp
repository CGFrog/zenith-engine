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

struct RenderSettings {
    AntialiasingMode antialiasing{AntialiasingMode::Fxaa};
    ShadowQuality shadows{ShadowQuality::Single};
    LightingQuality lighting{LightingQuality::PhysicallyBased};
    BloomQuality bloom{BloomQuality::Off};
    float cascadeSplitLambda{0.65f};
    float cascadeBlendFraction{0.10f};
    bool visualizeShadowCascades{false};
};

struct RenderCapabilities {
    bool descriptorIndexing{false};
    bool multiDrawIndirect{false};
    bool indirectDrawCount{false};
    bool gpuDrivenPath{false};
};

struct RenderStats {
    uint32_t renderableCount{0};
    uint32_t visibleCount{0};
    uint32_t culledCount{0};
    uint32_t shadowCasterCount{0};
    uint32_t drawCalls{0};
    uint64_t triangleCount{0};
    double gpuFrameMilliseconds{0.0};
    double gpuShadowMilliseconds{0.0};
    double gpuSceneMilliseconds{0.0};
    double gpuPostMilliseconds{0.0};
    double cpuRenderPacketMilliseconds{0.0};
    bool renderPacketsRebuilt{false};
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
