#include "ToneMapping.hpp"

#include <algorithm>
#include <cmath>

namespace {
// AgX (Troy Sobotka) using the widely used polynomial fit of Blender's sigmoid.
// Matrices are written column by column, matching the GLSL mat3 constructor.
const glm::mat3 agxInset{0.842479062253094f,  0.0423282422610123f, 0.0423756549057051f,
                         0.0784335999999992f, 0.878468636469772f,  0.0784336f,
                         0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f};
const glm::mat3 agxOutset{1.19687900512017f,    -0.0528968517574562f, -0.0529716355144438f,
                          -0.0980208811401368f, 1.15190312990417f,    -0.0980434501171241f,
                          -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f};
constexpr float agxMinimumEv = -12.47393f;
constexpr float agxMaximumEv = 4.026069f;
constexpr float agxDisplayGamma = 2.2f;

glm::vec3 agxContrast(const glm::vec3& x) {
    const glm::vec3 x2 = x * x;
    const glm::vec3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

glm::vec3 agxPunchyLook(const glm::vec3& color) {
    const float luma = relativeLuminance(color);
    const glm::vec3 contrasted = glm::pow(glm::max(color, glm::vec3(0.0f)), glm::vec3(1.35f));
    return glm::vec3(luma) + 1.4f * (contrasted - glm::vec3(luma));
}

glm::vec3 agx(const glm::vec3& color, bool punchy) {
    glm::vec3 encoded = agxInset * glm::max(color, glm::vec3(0.0f));
    encoded =
        glm::clamp(glm::log2(glm::max(encoded, glm::vec3(1e-10f))), glm::vec3(agxMinimumEv), glm::vec3(agxMaximumEv));
    encoded = (encoded - agxMinimumEv) / (agxMaximumEv - agxMinimumEv);
    encoded = agxContrast(encoded);
    if (punchy) {
        encoded = agxPunchyLook(encoded);
    }
    const glm::vec3 display = agxOutset * encoded;
    return glm::pow(glm::clamp(display, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(agxDisplayGamma));
}

glm::vec3 acesFit(const glm::vec3& color) {
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    return glm::clamp((color * (a * color + b)) / (color * (c * color + d) + e), glm::vec3(0.0f), glm::vec3(1.0f));
}
} // namespace

float relativeLuminance(const glm::vec3& linearColor) {
    return glm::dot(linearColor, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 toneMap(const glm::vec3& exposedColor, TonemapOperator tonemapOperator) {
    switch (tonemapOperator) {
    case TonemapOperator::Aces:
        return acesFit(exposedColor);
    case TonemapOperator::AgX:
        return agx(exposedColor, false);
    case TonemapOperator::AgXPunchy:
        return agx(exposedColor, true);
    }
    return agx(exposedColor, false);
}

uint32_t luminanceHistogramBin(float luminance, float minimumLogLuminance, float maximumLogLuminance) {
    const float logLuminance = std::log2(std::max(luminance, 1e-10f));
    const float normalized =
        std::clamp((logLuminance - minimumLogLuminance) / (maximumLogLuminance - minimumLogLuminance), 0.0f, 1.0f);
    return std::min(static_cast<uint32_t>(normalized * luminanceHistogramBinCount), luminanceHistogramBinCount - 1);
}

std::optional<float> histogramAverageLogLuminance(std::span<const uint32_t> bins, const ExposureSettings& settings) {
    float total = 0.0f;
    for (const uint32_t count : bins) {
        total += static_cast<float>(count);
    }
    if (total <= 0.0f || bins.empty()) {
        return std::nullopt;
    }
    const float low = settings.lowPercentile * total;
    const float high = settings.highPercentile * total;
    const float logRange = settings.maximumLogLuminance - settings.minimumLogLuminance;
    float cumulative = 0.0f;
    float weightedLogLuminance = 0.0f;
    float weight = 0.0f;
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const float before = cumulative;
        cumulative += static_cast<float>(bins[bin]);
        const float inside = std::clamp(cumulative, low, high) - std::clamp(before, low, high);
        const float binCenter = settings.minimumLogLuminance +
                                (static_cast<float>(bin) + 0.5f) / static_cast<float>(bins.size()) * logRange;
        weightedLogLuminance += inside * binCenter;
        weight += inside;
    }
    if (weight <= 0.0f) {
        // Degenerate percentile window: use the bin holding the low percentile.
        cumulative = 0.0f;
        for (size_t bin = 0; bin < bins.size(); ++bin) {
            cumulative += static_cast<float>(bins[bin]);
            if (cumulative >= low) {
                return settings.minimumLogLuminance +
                       (static_cast<float>(bin) + 0.5f) / static_cast<float>(bins.size()) * logRange;
            }
        }
        return std::nullopt;
    }
    return weightedLogLuminance / weight;
}

float autoExposureTarget(float averageLogLuminance, float compensationEv) {
    return exposureMiddleGrey / std::exp2(averageLogLuminance) * std::exp2(compensationEv);
}

float manualExposure(const ExposureSettings& settings) {
    return std::exp2(settings.manualEv + settings.compensationEv);
}

float adaptExposure(float previousExposure, float targetExposure, float deltaSeconds,
                    const ExposureSettings& settings) {
    if (!(previousExposure > 0.0f) || !std::isfinite(previousExposure)) {
        return targetExposure;
    }
    const float previousLog = std::log2(previousExposure);
    const float targetLog = std::log2(targetExposure);
    const float speed = targetLog > previousLog ? settings.adaptationSpeedUp : settings.adaptationSpeedDown;
    const float remaining = std::exp(-std::max(deltaSeconds, 0.0f) * speed);
    return std::exp2(targetLog + (previousLog - targetLog) * remaining);
}
