#pragma once

#include "Rendering.hpp"

#include <glm/glm.hpp>

struct TemporalFrameState {
    glm::vec2 jitterPixels{0.0f};
    glm::vec2 jitterNdc{0.0f};
    uint32_t internalWidth{1};
    uint32_t internalHeight{1};
    float resolutionScale{1.0f};
    bool historyValid{false};
    bool historyInvalidated{false};
    bool cameraCut{false};
};

struct TemporalResolveSample {
    glm::vec3 color{0.0f};
    float historyWeight{0.0f};
    bool disoccluded{false};
};

class TemporalQualityController {
  public:
    TemporalFrameState beginFrame(uint32_t outputWidth, uint32_t outputHeight, double gpuFrameMilliseconds,
                                  const DynamicResolutionSettings& resolution, bool temporalEnabled, bool cameraCut);
    void invalidateHistory();

    float smoothedGpuMilliseconds() const {
        return m_smoothedGpuMilliseconds;
    }

  private:
    float m_scale{1.0f};
    float m_smoothedGpuMilliseconds{0.0f};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_jitterIndex{0};
    uint32_t m_framesSinceScaleChange{0};
    bool m_historyValid{false};
    bool m_forceInvalidation{true};
};

TemporalResolveSample resolveTemporalSample(const glm::vec3& current, const glm::vec3& history,
                                            const glm::vec3& neighborhoodMinimum, const glm::vec3& neighborhoodMaximum,
                                            float motionPixels, float depthDifference, float reactiveMask);
