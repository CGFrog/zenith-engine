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

float luminance(const glm::vec3& color) {
    return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
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

    const uint32_t safeOutputWidth = std::max(outputWidth, 1u);
    const uint32_t safeOutputHeight = std::max(outputHeight, 1u);
    const uint32_t width = quantizedExtent(safeOutputWidth, requestedScale, resolution.quantization);
    const uint32_t height = quantizedExtent(safeOutputHeight, requestedScale, resolution.quantization);
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        m_scale = std::min(static_cast<float>(width) / safeOutputWidth, static_cast<float>(height) / safeOutputHeight);
        m_framesSinceScaleChange = 0;
    }
    // History is stored and reprojected at output resolution, so only an output
    // resize invalidates it; internal (dynamic) resolution changes do not.
    const bool outputChanged = safeOutputWidth != m_outputWidth || safeOutputHeight != m_outputHeight;
    m_outputWidth = safeOutputWidth;
    m_outputHeight = safeOutputHeight;

    const bool invalidated = m_forceInvalidation || cameraCut || outputChanged || !temporalEnabled;
    TemporalFrameState state{.internalWidth = width,
                             .internalHeight = height,
                             .resolutionScale = m_scale,
                             .historyValid = temporalEnabled && m_historyValid && !invalidated,
                             .historyInvalidated = invalidated,
                             .cameraCut = cameraCut};
    if (temporalEnabled) {
        // The jitter spans one internal pixel, so every internal resolution
        // covers its texel footprint over the 16-sample sequence.
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

glm::mat4 applyProjectionJitter(glm::mat4 projection, const glm::vec2& jitterNdc) {
    // Column 2 scales view-space z; with clip w = -z this shifts NDC by -jitterNdc.
    projection[2][0] += jitterNdc.x;
    projection[2][1] += jitterNdc.y;
    return projection;
}

glm::vec2 unjitteredMotionUv(const glm::vec4& currentClip, const glm::vec4& previousClip, const glm::vec4& jitterNdc) {
    const glm::vec2 currentNdc =
        glm::vec2(currentClip) / std::max(std::abs(currentClip.w), 0.00001f) + glm::vec2(jitterNdc.x, jitterNdc.y);
    const glm::vec2 previousNdc =
        glm::vec2(previousClip) / std::max(std::abs(previousClip.w), 0.00001f) + glm::vec2(jitterNdc.z, jitterNdc.w);
    return (currentNdc - previousNdc) * 0.5f;
}

glm::vec3 rgbToYCoCg(const glm::vec3& rgb) {
    return {glm::dot(rgb, glm::vec3(0.25f, 0.5f, 0.25f)), glm::dot(rgb, glm::vec3(0.5f, 0.0f, -0.5f)),
            glm::dot(rgb, glm::vec3(-0.25f, 0.5f, -0.25f))};
}

glm::vec3 yCoCgToRgb(const glm::vec3& yCoCg) {
    return {yCoCg.x + yCoCg.y - yCoCg.z, yCoCg.x + yCoCg.z, yCoCg.x - yCoCg.y - yCoCg.z};
}

float temporalReconstructionWeight(const glm::vec2& offsetInternalPixels) {
    return std::exp(-2.29f * glm::dot(offsetInternalPixels, offsetInternalPixels));
}

TemporalNeighborhood measureTemporalNeighborhood(std::span<const glm::vec3> rgbSamples) {
    if (rgbSamples.empty()) {
        return {};
    }
    glm::vec3 firstMoment(0.0f);
    glm::vec3 secondMoment(0.0f);
    for (const glm::vec3& sample : rgbSamples) {
        const glm::vec3 yCoCg = rgbToYCoCg(sample);
        firstMoment += yCoCg;
        secondMoment += yCoCg * yCoCg;
    }
    const float count = static_cast<float>(rgbSamples.size());
    const glm::vec3 mean = firstMoment / count;
    return {.mean = mean, .standardDeviation = glm::sqrt(glm::max(secondMoment / count - mean * mean, 0.0f))};
}

float temporalClipGamma(float motionPixels) {
    return std::lerp(1.25f, 1.0f, std::clamp(motionPixels / 8.0f, 0.0f, 1.0f));
}

glm::vec3 clipTemporalHistory(const glm::vec3& historyRgb, const TemporalNeighborhood& neighborhood, float gamma) {
    const glm::vec3 extent = glm::max(neighborhood.standardDeviation * gamma, glm::vec3(0.0001f));
    const glm::vec3 offset = rgbToYCoCg(historyRgb) - neighborhood.mean;
    const glm::vec3 units = glm::abs(offset / extent);
    const float largestUnit = std::max({units.x, units.y, units.z});
    if (largestUnit <= 1.0f) {
        return historyRgb;
    }
    return yCoCgToRgb(neighborhood.mean + offset / largestUnit);
}

float temporalHistoryWeight(float sampleConfidence, float motionPixels, float reactive) {
    float currentWeight = 0.1f * std::clamp(sampleConfidence, 0.0f, 1.0f);
    currentWeight = std::lerp(currentWeight, 0.5f, std::clamp(motionPixels / 32.0f, 0.0f, 1.0f));
    currentWeight = std::lerp(currentWeight, 1.0f, std::clamp(reactive, 0.0f, 1.0f));
    return 1.0f - currentWeight;
}

TemporalResolveSample resolveTemporalSample(const TemporalResolveInputs& inputs) {
    if (!inputs.historyAvailable) {
        return {.color = inputs.current, .historyWeight = 0.0f};
    }
    const glm::vec3 clipped =
        clipTemporalHistory(inputs.history, inputs.neighborhood, temporalClipGamma(inputs.motionPixels));
    const float historyWeight = temporalHistoryWeight(inputs.sampleConfidence, inputs.motionPixels, inputs.reactive);
    // Weighting by 1 / (1 + luma) blends in a tonemapped-like space, so single
    // bright samples cannot dominate the accumulated result.
    const float currentLumaWeight = (1.0f - historyWeight) / (1.0f + luminance(inputs.current));
    const float historyLumaWeight = historyWeight / (1.0f + luminance(clipped));
    const float totalWeight = std::max(currentLumaWeight + historyLumaWeight, 0.00001f);
    return {.color = (inputs.current * currentLumaWeight + clipped * historyLumaWeight) / totalWeight,
            .historyWeight = historyLumaWeight / totalWeight};
}
