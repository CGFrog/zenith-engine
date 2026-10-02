#include "ColorGrading.hpp"

#include <algorithm>
#include <cmath>

namespace color_grading {
namespace {
// PCG hash (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020).
uint32_t pcgHash(uint32_t value) {
    const uint32_t state = value * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float linearToSrgbChannel(float linear) {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    return clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
}

float srgbToLinearChannel(float encoded) {
    const float clamped = std::clamp(encoded, 0.0f, 1.0f);
    return clamped <= 0.04045f ? clamped / 12.92f : std::pow((clamped + 0.055f) / 1.055f, 2.4f);
}
} // namespace

bool lutRequested(const ColorLutSettings& settings) {
    return settings.enabled && !settings.path.empty() && settings.strength > 0.0f;
}

bool vignetteActive(const VignetteSettings& settings) {
    return settings.enabled && settings.intensity > 0.0f;
}

bool filmGrainActive(const FilmGrainSettings& settings) {
    return settings.enabled && settings.intensity > 0.0f;
}

glm::vec3 linearToSrgb(const glm::vec3& linear) {
    return {linearToSrgbChannel(linear.r), linearToSrgbChannel(linear.g), linearToSrgbChannel(linear.b)};
}

glm::vec3 srgbToLinear(const glm::vec3& encoded) {
    return {srgbToLinearChannel(encoded.r), srgbToLinearChannel(encoded.g), srgbToLinearChannel(encoded.b)};
}

glm::vec3 lutTextureCoordinate(const glm::vec3& encoded, const glm::vec3& domainMinimum, const glm::vec3& domainMaximum,
                               uint32_t size) {
    const glm::vec3 normalized =
        glm::clamp((encoded - domainMinimum) / (domainMaximum - domainMinimum), glm::vec3(0.0f), glm::vec3(1.0f));
    const float texels = static_cast<float>(size);
    return normalized * ((texels - 1.0f) / texels) + 0.5f / texels;
}

glm::vec3 sampleColorLut(const ColorLut& lut, const glm::vec3& encoded) {
    const glm::vec3 normalized = glm::clamp((encoded - lut.domainMinimum) / (lut.domainMaximum - lut.domainMinimum),
                                            glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 position = normalized * static_cast<float>(lut.size - 1);
    const glm::uvec3 lower = glm::min(glm::uvec3(glm::floor(position)), glm::uvec3(lut.size - 2));
    const glm::vec3 fraction = position - glm::vec3(lower);
    const auto at = [&](uint32_t red, uint32_t green, uint32_t blue) {
        return lut.texel(lower.x + red, lower.y + green, lower.z + blue);
    };
    const glm::vec3 green0 = glm::mix(glm::mix(at(0, 0, 0), at(1, 0, 0), fraction.x),
                                      glm::mix(at(0, 1, 0), at(1, 1, 0), fraction.x), fraction.y);
    const glm::vec3 green1 = glm::mix(glm::mix(at(0, 0, 1), at(1, 0, 1), fraction.x),
                                      glm::mix(at(0, 1, 1), at(1, 1, 1), fraction.x), fraction.y);
    return glm::mix(green0, green1, fraction.z);
}

float vignette(const glm::vec2& screenPosition, float aspectRatio, const VignetteSettings& settings) {
    // Roundness 1 measures distance in square (pixel-proportional) units, so
    // the falloff is circular on screen; roundness 0 follows the frame.
    const glm::vec2 scale{glm::mix(1.0f, aspectRatio, settings.roundness), 1.0f};
    const glm::vec2 offset = (screenPosition - 0.5f) * scale;
    const float distance = glm::length(offset) / glm::length(0.5f * scale);
    const float falloff = glm::smoothstep(1.0f - settings.smoothness, 1.0f, distance);
    return 1.0f - settings.intensity * falloff;
}

float filmGrainNoise(const glm::uvec2& cell, uint32_t frame) {
    const uint32_t hash = pcgHash(cell.x + pcgHash(cell.y + pcgHash(frame)));
    return static_cast<float>(hash) * (1.0f / 4294967296.0f) - 0.5f;
}

glm::vec3 applyColorGrade(const glm::vec3& displayLinear, const glm::uvec2& pixel, const glm::uvec2& extent,
                          uint32_t frame, const ColorLut* lut, const RenderSettings& settings) {
    glm::vec3 color = glm::clamp(displayLinear, glm::vec3(0.0f), glm::vec3(1.0f));
    if (lut != nullptr && lutRequested(settings.colorLut)) {
        const glm::vec3 encoded = linearToSrgb(color);
        const glm::vec3 graded = glm::mix(encoded, sampleColorLut(*lut, encoded), settings.colorLut.strength);
        color = srgbToLinear(graded);
    }
    if (vignetteActive(settings.vignette)) {
        const glm::vec2 screenPosition = (glm::vec2(pixel) + 0.5f) / glm::vec2(extent);
        color *=
            vignette(screenPosition, static_cast<float>(extent.x) / static_cast<float>(extent.y), settings.vignette);
    }
    if (filmGrainActive(settings.filmGrain)) {
        const glm::uvec2 cell{glm::floor(glm::vec2(pixel) / settings.filmGrain.size)};
        const glm::vec3 encoded = linearToSrgb(color) + settings.filmGrain.intensity * filmGrainNoise(cell, frame);
        color = srgbToLinear(encoded);
    }
    return color;
}
} // namespace color_grading
