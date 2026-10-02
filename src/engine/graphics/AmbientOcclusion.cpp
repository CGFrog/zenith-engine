#include "AmbientOcclusion.hpp"

#include <algorithm>
#include <cmath>

namespace ambient_occlusion {
namespace {
// Relative view-depth difference at which a denoise neighbour stops counting.
constexpr float denoiseDepthTolerance = 0.1f;
// Weight an upsampling texel across a depth edge keeps; mirrors gtao_upsample.comp.
constexpr float upsampleEdgeWeight = 1e-3f;

// Highest horizon cosine found along one side of a slice. `lowCosine` is the
// horizon of an unoccluded hemisphere on that side; samples fade towards it
// with distance so far geometry stops occluding.
float sideHorizonCosine(const GtaoSample& sample, const ViewPositionSource& positions, const GtaoParameters& parameters,
                        glm::vec2 screenDirection, float radiusPixels, float lowCosine) {
    const float falloffRange = std::max(parameters.falloffFraction * parameters.radius, 1e-4f);
    const float falloffMultiply = -1.0f / falloffRange;
    const float falloffAdd = (parameters.radius - falloffRange) / falloffRange + 1.0f;
    const glm::vec3 viewVector = glm::normalize(-sample.viewPosition);
    const glm::vec2 center = glm::vec2(sample.pixel) + 0.5f;
    float horizon = lowCosine;
    const uint32_t level = parameters.horizonDepthLevel;
    const float minimumOffset = minimumStepPixels(level);
    for (uint32_t step = 0; step < parameters.stepsPerSide; ++step) {
        float distance = (static_cast<float>(step) + sample.stepNoise) / static_cast<float>(parameters.stepsPerSide);
        // Quadratic spacing concentrates samples near the center.
        const float offsetPixels = std::max(distance * distance * radiusPixels, minimumOffset);
        const std::optional<glm::ivec2> texel =
            horizonTexel(center + screenDirection * offsetPixels, level, sample.renderExtent);
        if (!texel) {
            continue;
        }
        const std::optional<glm::vec3> samplePosition = positions(*texel, level);
        if (!samplePosition) {
            continue;
        }
        const glm::vec3 delta = *samplePosition - sample.viewPosition;
        const float length = glm::length(delta);
        if (length <= 1e-6f) {
            continue;
        }
        const float weight = std::clamp(length * falloffMultiply + falloffAdd, 0.0f, 1.0f);
        const float sampleCosine = glm::dot(delta / length, viewVector);
        horizon = std::max(horizon, lowCosine + (sampleCosine - lowCosine) * weight);
    }
    return horizon;
}
} // namespace

GtaoQualityProfile qualityProfile(AmbientOcclusionQuality quality) {
    if (quality == AmbientOcclusionQuality::HalfResolution) {
        // Half the texels in each direction, two slices instead of three, and
        // horizons from pyramid level 1, whose texels match the occlusion
        // texels' footprint. The 4x4 noise tile still gives the denoiser 32
        // slice directions per window.
        return {.resolutionScale = 0.5f, .parameters = {.sliceCount = 2, .stepsPerSide = 4, .horizonDepthLevel = 1}};
    }
    return {.resolutionScale = 1.0f, .parameters = {}};
}

glm::uvec2 occlusionExtent(glm::uvec2 renderExtent, float scale) {
    const glm::vec2 scaled = glm::ceil(glm::vec2(renderExtent) * scale);
    return glm::max(glm::uvec2(scaled), glm::uvec2(1u));
}

glm::uvec2 dispatchExtent(glm::uvec2 renderExtent, float scale, glm::uvec2 storageExtent) {
    return glm::min(occlusionExtent(renderExtent, scale) + 1u, storageExtent);
}

glm::ivec2 sourcePixel(glm::uvec2 texel, float scale, glm::uvec2 renderExtent) {
    const glm::ivec2 pixel(glm::floor(glm::vec2(texel) / scale));
    return glm::clamp(pixel, glm::ivec2(0), glm::ivec2(renderExtent) - 1);
}

glm::vec2 occlusionUv(glm::vec2 fragmentCoordinate, float scale, glm::uvec2 storageExtent) {
    return (fragmentCoordinate * scale + 0.5f * (1.0f - scale)) / glm::vec2(storageExtent);
}

std::optional<glm::ivec2> horizonTexel(glm::vec2 pixelPosition, uint32_t level, glm::uvec2 renderExtent) {
    const glm::ivec2 pixel(static_cast<int>(std::floor(pixelPosition.x)),
                           static_cast<int>(std::floor(pixelPosition.y)));
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= static_cast<int>(renderExtent.x) ||
        pixel.y >= static_cast<int>(renderExtent.y)) {
        return std::nullopt;
    }
    const glm::ivec2 levelExtent(glm::max(glm::uvec2(renderExtent.x >> level, renderExtent.y >> level), 1u));
    return glm::min(glm::ivec2(pixel.x >> level, pixel.y >> level), levelExtent - 1);
}

glm::vec2 horizonTexelCenter(glm::ivec2 texel, uint32_t level) {
    return (glm::vec2(texel) + 0.5f) * static_cast<float>(1u << level);
}

bool horizonUsesClosestDepth(glm::ivec2 texel) {
    return ((texel.x + texel.y) & 1) != 0;
}

float minimumStepPixels(uint32_t level) {
    // 1.5 texels reaches the next texel along any direction, diagonals included.
    return level == 0 ? 1.0f : 1.5f * static_cast<float>(1u << level);
}

glm::vec3 reconstructViewPosition(const glm::mat4& inverseProjection, glm::vec2 pixel, glm::uvec2 renderExtent,
                                  float depth) {
    const glm::vec2 ndc = pixel / glm::vec2(renderExtent) * 2.0f - 1.0f;
    const glm::vec4 view = inverseProjection * glm::vec4(ndc, depth, 1.0f);
    return glm::vec3(view) / view.w;
}

float sliceArcVisibility(float h0, float h1, float n) {
    const float cosN = std::cos(n);
    const float sinN = std::sin(n);
    return 0.25f * (-std::cos(2.0f * h0 - n) + cosN + 2.0f * h0 * sinN) +
           0.25f * (-std::cos(2.0f * h1 - n) + cosN + 2.0f * h1 * sinN);
}

float integrateVisibility(const GtaoSample& sample, const ViewPositionSource& positions,
                          const GtaoParameters& parameters) {
    const float viewDistance = -sample.viewPosition.z;
    if (viewDistance <= 0.0f || parameters.sliceCount == 0) {
        return 1.0f;
    }
    const float radiusPixels = parameters.radius * sample.projectionScale / viewDistance;
    if (radiusPixels < 1.0f) {
        return 1.0f;
    }
    const glm::vec3 viewVector = glm::normalize(-sample.viewPosition);
    const glm::vec3 normal = glm::normalize(sample.viewNormal);
    float visibility = 0.0f;
    for (uint32_t slice = 0; slice < parameters.sliceCount; ++slice) {
        const float angle =
            (static_cast<float>(slice) + sample.sliceNoise) * pi / static_cast<float>(parameters.sliceCount);
        // Screen space points down, view space up.
        const glm::vec2 screenDirection(std::cos(angle), std::sin(angle));
        const glm::vec3 direction(screenDirection.x, -screenDirection.y, 0.0f);
        const glm::vec3 orthogonalDirection = direction - viewVector * glm::dot(direction, viewVector);
        const glm::vec3 axis = glm::normalize(glm::cross(direction, viewVector));
        const glm::vec3 projectedNormal = normal - axis * glm::dot(normal, axis);
        const float projectedLength = glm::length(projectedNormal);
        if (projectedLength <= 1e-6f) {
            continue;
        }
        // Angles are measured from the view vector, positive towards +direction.
        const float cosN = std::clamp(glm::dot(projectedNormal, viewVector) / projectedLength, 0.0f, 1.0f);
        const float n = (glm::dot(projectedNormal, orthogonalDirection) < 0.0f ? -1.0f : 1.0f) * std::acos(cosN);
        const float positiveCosine =
            sideHorizonCosine(sample, positions, parameters, screenDirection, radiusPixels, std::cos(n + 0.5f * pi));
        const float negativeCosine =
            sideHorizonCosine(sample, positions, parameters, -screenDirection, radiusPixels, std::cos(n - 0.5f * pi));
        float h1 = std::acos(std::clamp(positiveCosine, -1.0f, 1.0f));
        float h0 = -std::acos(std::clamp(negativeCosine, -1.0f, 1.0f));
        h0 = n + std::max(h0 - n, -0.5f * pi);
        h1 = n + std::min(h1 - n, 0.5f * pi);
        visibility += sliceDensityWeight(direction, viewVector) * projectedLength * sliceArcVisibility(h0, h1, n);
    }
    return std::max(visibility / static_cast<float>(parameters.sliceCount), 0.0f);
}

float applyOcclusionPower(float visibility, const GtaoParameters& parameters) {
    return std::pow(std::clamp(visibility, 0.0f, 1.0f), parameters.occlusionPower);
}

float specularOcclusion(float nDotV, float ambientOcclusion, float roughness) {
    const float exponent = std::exp2(-16.0f * roughness - 1.0f);
    return std::clamp(std::pow(nDotV + ambientOcclusion, exponent) - 1.0f + ambientOcclusion, 0.0f, 1.0f);
}

float sliceDensityWeight(glm::vec3 direction, glm::vec3 viewVector) {
    const float alignment = glm::dot(direction, viewVector);
    return std::abs(viewVector.z) / std::max(1.0f - alignment * alignment, 1e-4f);
}

float denoiseSpatialWeight(int offset) {
    return std::abs(offset) == denoiseRadius ? 0.5f : 1.0f;
}

float denoiseDepthWeight(float centerDepth, float sampleDepth) {
    const float tolerance = std::max(centerDepth * denoiseDepthTolerance, 1e-4f);
    return std::clamp(1.0f - std::abs(sampleDepth - centerDepth) / tolerance, 0.0f, 1.0f);
}

glm::ivec2 upsampleWindowOrigin(glm::ivec2 pixel) {
    return glm::ivec2(pixel.x >> 1, pixel.y >> 1) - 1;
}

float upsampleDepthWeight(float pixelDepth, float sampleDepth) {
    return denoiseDepthWeight(pixelDepth, sampleDepth) + upsampleEdgeWeight;
}

} // namespace ambient_occlusion
