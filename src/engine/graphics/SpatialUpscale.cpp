// The EASU and RCAS reference functions port AMD FidelityFX Super Resolution
// 1.0 (ffx_fsr1.h), Copyright (c) 2021 Advanced Micro Devices, Inc., released
// under the MIT License. See shaders/fsr_easu.comp for the full notice.
#include "SpatialUpscale.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Divisors below this are treated as this, so flat regions stay finite.
constexpr float minimumDivisor = 1.0e-8f;
// RCAS limits its lobe so it cannot clip a ring of equal values.
constexpr float rcasLimit = 0.25f - 1.0f / 16.0f;
// Each sharpness unit below 1 attenuates RCAS by this many stops.
constexpr float rcasStopsPerUnit = 2.0f;

float approximateLuma(const glm::vec3& color) {
    // Luma times two in two multiply-adds, as EASU and RCAS use it.
    return color.b * 0.5f + (color.r * 0.5f + color.g);
}

float safeReciprocal(float value) {
    return 1.0f / std::max(value, minimumDivisor);
}

// Accumulates the edge direction and length of one bilinear corner of the
// f-g-j-k quad from its '+' neighborhood (above, left, center, right, below).
void accumulateEdge(glm::vec2& direction, float& length, float weight, float above, float left, float center,
                    float right, float below) {
    const float horizontalSpan = std::max(std::abs(right - center), std::abs(center - left));
    const float horizontalDelta = right - left;
    direction.x += horizontalDelta * weight;
    float horizontalLength = std::clamp(std::abs(horizontalDelta) * safeReciprocal(horizontalSpan), 0.0f, 1.0f);
    length += horizontalLength * horizontalLength * weight;

    const float verticalSpan = std::max(std::abs(below - center), std::abs(center - above));
    const float verticalDelta = below - above;
    direction.y += verticalDelta * weight;
    float verticalLength = std::clamp(std::abs(verticalDelta) * safeReciprocal(verticalSpan), 0.0f, 1.0f);
    length += verticalLength * verticalLength * weight;
}

struct LanczosWindow {
    glm::vec2 direction;
    glm::vec2 anisotropy;
    float lobe;
    float clip;
};

// Approximate Lanczos-2 weight of a tap at the given offset (input pixels)
// after rotating into the edge direction and stretching anisotropically.
float lanczosWeight(glm::vec2 offset, const LanczosWindow& window) {
    glm::vec2 rotated{offset.x * window.direction.x + offset.y * window.direction.y,
                      offset.x * -window.direction.y + offset.y * window.direction.x};
    rotated *= window.anisotropy;
    const float distanceSquared = std::min(glm::dot(rotated, rotated), window.clip);
    // (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lobe * x^2 - 1)^2
    float base = 2.0f / 5.0f * distanceSquared - 1.0f;
    float windowTerm = window.lobe * distanceSquared - 1.0f;
    base *= base;
    windowTerm *= windowTerm;
    base = 25.0f / 16.0f * base - (25.0f / 16.0f - 1.0f);
    return base * windowTerm;
}
} // namespace

float upscalerScaleFactor(UpscalerQuality quality) {
    switch (quality) {
    case UpscalerQuality::Quality:
        return 1.5f;
    case UpscalerQuality::Balanced:
        return 1.7f;
    case UpscalerQuality::Performance:
        return 2.0f;
    }
    return 1.0f;
}

float upscalerRenderScale(const UpscalerSettings& upscaler) {
    return upscaler.mode == UpscalerMode::Off ? 1.0f : 1.0f / upscalerScaleFactor(upscaler.quality);
}

DynamicResolutionSettings upscaledDynamicResolution(const DynamicResolutionSettings& resolution,
                                                    const UpscalerSettings& upscaler) {
    DynamicResolutionSettings upscaled = resolution;
    if (upscaler.mode == UpscalerMode::Off) {
        return upscaled;
    }
    const float presetScale = upscalerRenderScale(upscaler);
    if (!upscaled.enabled) {
        upscaled.enabled = true;
        upscaled.minimumScale = presetScale;
        upscaled.maximumScale = presetScale;
        return upscaled;
    }
    upscaled.minimumScale *= presetScale;
    upscaled.maximumScale *= presetScale;
    return upscaled;
}

EasuConstants easuConstants(glm::uvec2 inputViewport, glm::uvec2 outputSize) {
    const glm::vec2 input = glm::max(glm::vec2(inputViewport), glm::vec2(1.0f));
    const glm::vec2 output = glm::max(glm::vec2(outputSize), glm::vec2(1.0f));
    const glm::vec2 scale = input / output;
    return {.scale = scale,
            .offset = 0.5f * scale - 0.5f,
            .inputMaximum = glm::ivec2(input) - 1,
            .outputSize = glm::ivec2(output)};
}

glm::vec3 easuReconstruct(const EasuTaps& taps, glm::vec2 fraction) {
    const float bL = approximateLuma(taps.b);
    const float cL = approximateLuma(taps.c);
    const float eL = approximateLuma(taps.e);
    const float fL = approximateLuma(taps.f);
    const float gL = approximateLuma(taps.g);
    const float hL = approximateLuma(taps.h);
    const float iL = approximateLuma(taps.i);
    const float jL = approximateLuma(taps.j);
    const float kL = approximateLuma(taps.k);
    const float lL = approximateLuma(taps.l);
    const float nL = approximateLuma(taps.n);
    const float oL = approximateLuma(taps.o);

    // Bilinear blend of the edge analysis at the four quad corners.
    glm::vec2 direction{0.0f};
    float length = 0.0f;
    const float x = fraction.x;
    const float y = fraction.y;
    accumulateEdge(direction, length, (1.0f - x) * (1.0f - y), bL, eL, fL, gL, jL);
    accumulateEdge(direction, length, x * (1.0f - y), cL, fL, gL, hL, kL);
    accumulateEdge(direction, length, (1.0f - x) * y, fL, iL, jL, kL, nL);
    accumulateEdge(direction, length, x * y, gL, jL, kL, lL, oL);

    const float directionSquared = glm::dot(direction, direction);
    const bool flat = directionSquared < 1.0f / 32768.0f;
    const float inverseLength = flat ? 1.0f : 1.0f / std::sqrt(directionSquared);
    if (flat) {
        direction.x = 1.0f;
    }
    direction *= inverseLength;
    // From {0 to 2} to {0 to 1}, shaped with a square.
    length *= 0.5f;
    length *= length;
    // 1 for axis-aligned edges, sqrt(2) on diagonals.
    const float stretch =
        glm::dot(direction, direction) * safeReciprocal(std::max(std::abs(direction.x), std::abs(direction.y)));
    const glm::vec2 anisotropy{1.0f + (stretch - 1.0f) * length, 1.0f - 0.5f * length};
    // The window shifts from +/-sqrt(2) toward slightly beyond 2 on edges.
    const float lobe = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * length;
    const LanczosWindow window{.direction = direction, .anisotropy = anisotropy, .lobe = lobe, .clip = 1.0f / lobe};

    glm::vec3 color{0.0f};
    float totalWeight = 0.0f;
    const auto tap = [&](glm::vec2 texelOffset, const glm::vec3& tapColor) {
        const float weight = lanczosWeight(texelOffset - fraction, window);
        color += tapColor * weight;
        totalWeight += weight;
    };
    tap({0.0f, -1.0f}, taps.b);
    tap({1.0f, -1.0f}, taps.c);
    tap({-1.0f, 1.0f}, taps.i);
    tap({0.0f, 1.0f}, taps.j);
    tap({0.0f, 0.0f}, taps.f);
    tap({-1.0f, 0.0f}, taps.e);
    tap({1.0f, 1.0f}, taps.k);
    tap({2.0f, 1.0f}, taps.l);
    tap({2.0f, 0.0f}, taps.h);
    tap({1.0f, 0.0f}, taps.g);
    tap({1.0f, 2.0f}, taps.o);
    tap({0.0f, 2.0f}, taps.n);

    // Dering: stay within the four nearest taps.
    const glm::vec3 minimum4 = glm::min(glm::min(taps.f, taps.g), glm::min(taps.j, taps.k));
    const glm::vec3 maximum4 = glm::max(glm::max(taps.f, taps.g), glm::max(taps.j, taps.k));
    const glm::vec3 filtered = totalWeight > minimumDivisor ? color / totalWeight : taps.f;
    return glm::min(maximum4, glm::max(minimum4, filtered));
}

float rcasSharpnessStops(float sharpness) {
    return (1.0f - std::clamp(sharpness, 0.0f, 1.0f)) * rcasStopsPerUnit;
}

float rcasLobeScale(float sharpness) {
    return std::exp2(-rcasSharpnessStops(sharpness));
}

glm::vec3 rcasSharpen(const glm::vec3& north, const glm::vec3& west, const glm::vec3& center, const glm::vec3& east,
                      const glm::vec3& south, float lobeScale) {
    const float northLuma = approximateLuma(north);
    const float westLuma = approximateLuma(west);
    const float centerLuma = approximateLuma(center);
    const float eastLuma = approximateLuma(east);
    const float southLuma = approximateLuma(south);
    // Noise detection: a center that stands out from an otherwise flat ring
    // is attenuated, so grain is not amplified.
    const float lumaRange = std::max({northLuma, westLuma, centerLuma, eastLuma, southLuma}) -
                            std::min({northLuma, westLuma, centerLuma, eastLuma, southLuma});
    float noise = 0.25f * (northLuma + westLuma + eastLuma + southLuma) - centerLuma;
    noise = std::clamp(std::abs(noise) * safeReciprocal(lumaRange), 0.0f, 1.0f);
    noise = 1.0f - 0.5f * noise;

    const glm::vec3 ringMinimum = glm::min(glm::min(north, west), glm::min(east, south));
    const glm::vec3 ringMaximum = glm::max(glm::max(north, west), glm::max(east, south));
    // The most negative lobe that keeps every channel inside [0, 1] given
    // the ring's range (the "hit" limiters of FsrRcasF).
    float lobe = -rcasLimit;
    float strongestLobe = -1.0f;
    for (int channel = 0; channel < 3; ++channel) {
        const float hitMinimum =
            std::min(ringMinimum[channel], center[channel]) * safeReciprocal(4.0f * ringMaximum[channel]);
        const float hitMaximum = (1.0f - std::max(ringMaximum[channel], center[channel])) /
                                 std::min(4.0f * ringMinimum[channel] - 4.0f, -minimumDivisor);
        strongestLobe = std::max(strongestLobe, std::max(-hitMinimum, hitMaximum));
    }
    lobe = std::max(lobe, std::min(strongestLobe, 0.0f)) * lobeScale * noise;
    return (lobe * (north + west + east + south) + center) / (4.0f * lobe + 1.0f);
}
