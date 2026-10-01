#include "TemporalQuality.hpp"

#include <algorithm>
#include <cmath>

namespace {
float halton(uint32_t index, uint32_t base) {
    float value = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return value;
}

uint32_t quantizedExtent(uint32_t output, float scale, uint32_t quantum) {
    const uint32_t safeQuantum = std::max(quantum, 1u);
    const uint32_t scaled = std::max(static_cast<uint32_t>(std::lround(static_cast<float>(output) * scale)), 1u);
    return std::min(((scaled + safeQuantum - 1) / safeQuantum) * safeQuantum, output);
}
} // namespace

TemporalFrameState TemporalQualityController::beginFrame(uint32_t outputWidth, uint32_t outputHeight,
                                                         double gpuFrameMilliseconds,
                                                         const DynamicResolutionSettings& resolution,
                                                         bool temporalEnabled, bool cameraCut) {
    if (gpuFrameMilliseconds > 0.0) {
        const float sample = static_cast<float>(gpuFrameMilliseconds);
        m_smoothedGpuMilliseconds =
            m_smoothedGpuMilliseconds == 0.0f ? sample : std::lerp(m_smoothedGpuMilliseconds, sample, 0.1f);
    }

    float requestedScale = 1.0f;
    if (resolution.enabled) {
        requestedScale = std::clamp(m_scale, resolution.minimumScale, resolution.maximumScale);
        ++m_framesSinceScaleChange;
        const float target = std::max(resolution.targetGpuMilliseconds, 0.1f);
        const float normalizedError = (m_smoothedGpuMilliseconds - target) / target;
        if (m_smoothedGpuMilliseconds > 0.0f && m_framesSinceScaleChange >= 8 && std::abs(normalizedError) > 0.05f) {
            const float direction = normalizedError > 0.0f ? -1.0f : 1.0f;
            requestedScale = std::clamp(requestedScale + direction * resolution.adjustmentRate, resolution.minimumScale,
                                        resolution.maximumScale);
        }
    }

    const uint32_t width = quantizedExtent(std::max(outputWidth, 1u), requestedScale, resolution.quantization);
    const uint32_t height = quantizedExtent(std::max(outputHeight, 1u), requestedScale, resolution.quantization);
    const bool extentChanged = width != m_width || height != m_height;
    if (extentChanged) {
        m_width = width;
        m_height = height;
        m_scale = std::min(static_cast<float>(width) / std::max(outputWidth, 1u),
                           static_cast<float>(height) / std::max(outputHeight, 1u));
        m_framesSinceScaleChange = 0;
    }

    const bool invalidated = m_forceInvalidation || cameraCut || extentChanged || !temporalEnabled;
    TemporalFrameState state{.internalWidth = width,
                             .internalHeight = height,
                             .resolutionScale = m_scale,
                             .historyValid = temporalEnabled && m_historyValid && !invalidated,
                             .historyInvalidated = invalidated,
                             .cameraCut = cameraCut};
    if (temporalEnabled) {
        const uint32_t sampleIndex = (m_jitterIndex++ % 16) + 1;
        state.jitterPixels = {halton(sampleIndex, 2) - 0.5f, halton(sampleIndex, 3) - 0.5f};
        state.jitterNdc = {state.jitterPixels.x * 2.0f / static_cast<float>(width),
                           state.jitterPixels.y * 2.0f / static_cast<float>(height)};
        m_historyValid = true;
    } else {
        m_jitterIndex = 0;
        m_historyValid = false;
    }
    m_forceInvalidation = false;
    return state;
}

void TemporalQualityController::invalidateHistory() {
    m_forceInvalidation = true;
    m_historyValid = false;
}

TemporalResolveSample resolveTemporalSample(const glm::vec3& current, const glm::vec3& history,
                                            const glm::vec3& neighborhoodMinimum, const glm::vec3& neighborhoodMaximum,
                                            float motionPixels, float depthDifference, float reactiveMask) {
    const bool disoccluded = depthDifference > 0.01f || motionPixels > 128.0f;
    const glm::vec3 clippedHistory = glm::clamp(history, neighborhoodMinimum, neighborhoodMaximum);
    const float motionResponse = std::clamp(motionPixels / 32.0f, 0.0f, 1.0f);
    const float reactiveResponse = std::clamp(reactiveMask, 0.0f, 1.0f);
    const float historyWeight =
        disoccluded ? 0.0f : 0.92f * (1.0f - 0.75f * motionResponse) * (1.0f - reactiveResponse);
    return {.color = glm::mix(current, clippedHistory, historyWeight),
            .historyWeight = historyWeight,
            .disoccluded = disoccluded};
}
