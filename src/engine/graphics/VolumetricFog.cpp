#include "VolumetricFog.hpp"

#include <algorithm>
#include <cmath>

namespace volumetric_fog {
namespace {
constexpr float pi = 3.14159265358979f;
// Below this extinction a slice is treated as empty of absorbers, where the
// analytic integral degenerates to scattering * length.
constexpr float minimumExtinction = 1.0e-6f;
} // namespace

glm::uvec3 froxelSubdivision(VolumetricFogQuality quality) {
    // With the default 16x9x24 cluster grid: Low 80x45x48, Medium 160x90x72,
    // High 160x90x96 froxels.
    switch (quality) {
    case VolumetricFogQuality::Off:
        return glm::uvec3(0u);
    case VolumetricFogQuality::Low:
        return {5u, 5u, 2u};
    case VolumetricFogQuality::Medium:
        return {10u, 10u, 3u};
    case VolumetricFogQuality::High:
        return {10u, 10u, 4u};
    }
    return glm::uvec3(0u);
}

bool temporalReprojection(VolumetricFogQuality quality) {
    return quality == VolumetricFogQuality::Medium || quality == VolumetricFogQuality::High;
}

glm::uvec3 FroxelGrid::dimensions() const {
    const ClusterGridConfiguration& configuration = clusters.configuration;
    return glm::uvec3(configuration.columns, configuration.rows, configuration.depthSlices) * subdivision;
}

glm::vec2 FroxelGrid::sliceScaleBias() const {
    const float scale = static_cast<float>(dimensions().z) / std::log(clusters.sliceEndDepth / clusters.nearDepth);
    return {scale, -std::log(clusters.nearDepth) * scale};
}

float FroxelGrid::sliceCoordinate(float viewDepth) const {
    const glm::vec2 scaleBias = sliceScaleBias();
    const float slice = std::log(std::max(viewDepth, clusters.nearDepth)) * scaleBias.x + scaleBias.y;
    return std::clamp(slice, 0.0f, static_cast<float>(dimensions().z));
}

float FroxelGrid::sliceDepth(float slice) const {
    const float fraction = slice / static_cast<float>(dimensions().z);
    return clusters.nearDepth * std::pow(clusters.sliceEndDepth / clusters.nearDepth, fraction);
}

glm::uvec3 FroxelGrid::clusterOf(const glm::uvec3& froxel) const {
    const ClusterGridConfiguration& configuration = clusters.configuration;
    const glm::uvec3 cluster = froxel / subdivision;
    return glm::min(cluster, glm::uvec3(configuration.columns, configuration.rows, configuration.depthSlices) - 1u);
}

float FroxelGrid::integratedW(float viewDepth) const {
    const float slices = static_cast<float>(dimensions().z);
    const float w = (sliceCoordinate(viewDepth) - 0.5f) / slices;
    return std::clamp(w, 0.5f / slices, (slices - 0.5f) / slices);
}

FroxelGrid makeFroxelGrid(const ClusterGrid& clusters, const glm::uvec3& subdivision) {
    return {.clusters = clusters, .subdivision = glm::max(subdivision, glm::uvec3(1u))};
}

float henyeyGreenstein(float cosine, float anisotropy) {
    const float g = std::clamp(anisotropy, -0.99f, 0.99f);
    const float denominator = 1.0f + g * g - 2.0f * g * std::clamp(cosine, -1.0f, 1.0f);
    return (1.0f - g * g) / (4.0f * pi * denominator * std::sqrt(denominator));
}

float heightFogExtinction(const VolumetricFogComponent& fog, float worldHeight) {
    const float aboveBase = std::max(worldHeight - fog.baseHeight, 0.0f);
    return std::max(fog.density, 0.0f) * std::exp(-std::max(fog.heightFalloff, 0.0f) * aboveBase);
}

std::vector<IntegratedFroxel> integrateFroxelColumn(std::span<const FroxelMedium> media,
                                                    std::span<const float> segmentLengths) {
    const size_t count = std::min(media.size(), segmentLengths.size());
    std::vector<IntegratedFroxel> column(count);
    glm::vec3 scattering(0.0f);
    float transmittance = 1.0f;
    for (size_t slice = 0; slice < count; ++slice) {
        const FroxelMedium& medium = media[slice];
        const float length = std::max(segmentLengths[slice], 0.0f);
        const float extinction = std::max(medium.extinction, 0.0f);
        const float sliceTransmittance = std::exp(-extinction * length);
        // Integral over the slice of inScattering * exp(-extinction * t).
        const glm::vec3 sliceScattering = extinction > minimumExtinction
                                              ? medium.inScattering * (1.0f - sliceTransmittance) / extinction
                                              : medium.inScattering * length;
        scattering += transmittance * sliceScattering;
        transmittance *= sliceTransmittance;
        column[slice] = {.scattering = scattering, .transmittance = transmittance};
    }
    return column;
}

} // namespace volumetric_fog
