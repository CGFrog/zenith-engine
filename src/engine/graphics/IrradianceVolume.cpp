#include "IrradianceVolume.hpp"

#include "EnvironmentLighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace irradiance_volume {
namespace {
// Smallest depth variance (world units squared) used by the Chebyshev test, so
// flat walls seen straight on still fade smoothly.
constexpr float minimumDepthVariance = 0.0001f;
// Weights below this are crushed toward zero so barely visible probes do not
// leak light (DDGI's "crush" threshold).
constexpr float weightCrushThreshold = 0.2f;

glm::vec2 signNotZero(const glm::vec2& value) {
    return {value.x >= 0.0f ? 1.0f : -1.0f, value.y >= 0.0f ? 1.0f : -1.0f};
}

glm::vec2 depthTexel(const IrradianceProbe& probe, int x, int y) {
    const int last = static_cast<int>(irradianceProbeDepthResolution) - 1;
    x = std::clamp(x, 0, last);
    y = std::clamp(y, 0, last);
    return probe.depthMoments[static_cast<size_t>(y) * irradianceProbeDepthResolution + static_cast<size_t>(x)];
}
} // namespace

glm::vec2 octahedralEncode(const glm::vec3& direction) {
    const float sum = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    glm::vec2 projected = sum > 0.0f ? glm::vec2(direction.x, direction.z) / sum : glm::vec2(0.0f);
    if (direction.y < 0.0f) {
        projected = (1.0f - glm::abs(glm::vec2(projected.y, projected.x))) * signNotZero(projected);
    }
    return projected * 0.5f + 0.5f;
}

glm::vec3 octahedralDecode(const glm::vec2& coordinate) {
    glm::vec2 projected = coordinate * 2.0f - 1.0f;
    const float height = 1.0f - std::abs(projected.x) - std::abs(projected.y);
    if (height < 0.0f) {
        projected = (1.0f - glm::abs(glm::vec2(projected.y, projected.x))) * signNotZero(projected);
    }
    return glm::normalize(glm::vec3(projected.x, height, projected.y));
}

glm::vec3 depthTexelDirection(uint32_t texelX, uint32_t texelY) {
    return octahedralDecode((glm::vec2(texelX, texelY) + 0.5f) / static_cast<float>(irradianceProbeDepthResolution));
}

glm::vec2 sampleDepthMoments(const IrradianceProbe& probe, const glm::vec3& direction) {
    const glm::vec2 texel =
        octahedralEncode(direction) * static_cast<float>(irradianceProbeDepthResolution) - glm::vec2(0.5f);
    const glm::vec2 base = glm::floor(texel);
    const glm::vec2 fraction = texel - base;
    const int x = static_cast<int>(base.x);
    const int y = static_cast<int>(base.y);
    const glm::vec2 top = glm::mix(depthTexel(probe, x, y), depthTexel(probe, x + 1, y), fraction.x);
    const glm::vec2 bottom = glm::mix(depthTexel(probe, x, y + 1), depthTexel(probe, x + 1, y + 1), fraction.x);
    return glm::mix(top, bottom, fraction.y);
}

float chebyshevVisibility(const glm::vec2& moments, float distance) {
    if (distance <= moments.x) {
        return 1.0f;
    }
    const float variance = std::max(moments.y - moments.x * moments.x, minimumDepthVariance);
    const float delta = distance - moments.x;
    const float probability = variance / (variance + delta * delta);
    return probability * probability * probability;
}

std::array<ProbeCorner, 8> surroundingProbes(const IrradianceVolumeData& data, const glm::vec3& position) {
    const glm::vec3 lastProbe = glm::vec3(data.probeCounts) - 1.0f;
    const glm::vec3 lattice =
        glm::clamp((position - data.minimum) / data.cellSize() - 0.5f, glm::vec3(0.0f), lastProbe);
    const glm::vec3 base = glm::min(glm::floor(lattice), glm::max(lastProbe - 1.0f, glm::vec3(0.0f)));
    const glm::vec3 alpha = glm::clamp(lattice - base, glm::vec3(0.0f), glm::vec3(1.0f));
    std::array<ProbeCorner, 8> corners;
    for (uint32_t corner = 0; corner < corners.size(); ++corner) {
        const glm::uvec3 offset(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        const glm::vec3 trilinear = glm::mix(1.0f - alpha, alpha, glm::vec3(offset));
        corners[corner] = {.index = glm::min(glm::uvec3(base) + offset, data.probeCounts - 1u),
                           .trilinear = trilinear.x * trilinear.y * trilinear.z};
    }
    return corners;
}

float edgeWeight(const IrradianceVolumeData& data, const glm::vec3& position, float blendDistance) {
    const glm::vec3 inside = glm::min(position - data.minimum, data.maximum - position);
    const float distance = std::min({inside.x, inside.y, inside.z});
    if (distance < 0.0f) {
        return 0.0f;
    }
    return blendDistance <= 0.0f ? 1.0f : std::min(distance / blendDistance, 1.0f);
}

Sample sample(const IrradianceVolumeData& data, const ShadingParameters& parameters, const glm::vec3& position,
              const glm::vec3& normal, const glm::vec3& viewDirection) {
    const glm::vec3 biased = position + normal * parameters.normalBias + viewDirection * parameters.viewBias;
    glm::vec3 irradiance(0.0f);
    float totalWeight = 0.0f;
    for (const ProbeCorner& corner : surroundingProbes(data, biased)) {
        const IrradianceProbe& probe = data.probes[data.probeIndex(corner.index)];
        if (!probe.valid || corner.trilinear <= 0.0f) {
            continue;
        }
        const glm::vec3 probePosition = data.probePosition(corner.index);
        const glm::vec3 toProbe = probePosition - position;
        const float probeDistance = glm::length(toProbe);
        const glm::vec3 directionToProbe = probeDistance > 0.0001f ? toProbe / probeDistance : normal;
        const float facing = (glm::dot(directionToProbe, normal) + 1.0f) * 0.5f;
        float weight = facing * facing + 0.2f;
        const glm::vec3 probeToPoint = biased - probePosition;
        const float pointDistance = glm::length(probeToPoint);
        const glm::vec3 direction = pointDistance > 0.0001f ? probeToPoint / pointDistance : normal;
        weight *= chebyshevVisibility(sampleDepthMoments(probe, direction), pointDistance);
        weight = std::max(weight, 0.000001f);
        if (weight < weightCrushThreshold) {
            weight *= weight * weight / (weightCrushThreshold * weightCrushThreshold);
        }
        weight *= corner.trilinear;
        environment_lighting::IrradianceCoefficients coefficients;
        std::copy(probe.irradiance.begin(), probe.irradiance.end(), coefficients.begin());
        irradiance += environment_lighting::evaluateIrradiance(coefficients, normal) * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0f) {
        return {};
    }
    return {.irradiance = irradiance / totalWeight,
            .volumeWeight = edgeWeight(data, position, parameters.blendDistance)};
}

Entity selectVolume(const Scene& scene, const glm::vec3& cameraPosition) {
    Entity selected = entt::null;
    float selectedDistance = std::numeric_limits<float>::max();
    uint64_t selectedId = 0;
    const auto volumes = scene.registry().view<const IrradianceVolumeComponent>();
    for (Entity entity : volumes) {
        const IrradianceVolumeComponent& volume = volumes.get<const IrradianceVolumeComponent>(entity);
        if (!volume.bakedData) {
            continue;
        }
        const glm::vec3 center = (volume.bakedData->minimum + volume.bakedData->maximum) * 0.5f;
        const glm::vec3 halfExtents = (volume.bakedData->maximum - volume.bakedData->minimum) * 0.5f;
        const float distance = glm::length(glm::max(glm::abs(cameraPosition - center) - halfExtents, glm::vec3(0.0f)));
        const uint64_t id = scene.id(entity).value;
        if (distance < selectedDistance || (distance == selectedDistance && id < selectedId)) {
            selected = entity;
            selectedDistance = distance;
            selectedId = id;
        }
    }
    return selected;
}

std::vector<GpuProbe> packProbes(const IrradianceVolumeData& data) {
    std::vector<GpuProbe> packed(data.probes.size());
    for (size_t index = 0; index < data.probes.size(); ++index) {
        const IrradianceProbe& probe = data.probes[index];
        for (size_t coefficient = 0; coefficient < probe.irradiance.size(); ++coefficient) {
            packed[index].irradiance[coefficient] = glm::vec4(probe.irradiance[coefficient], 0.0f);
        }
        packed[index].irradiance[0].w = probe.valid ? 1.0f : 0.0f;
        packed[index].depthMoments = probe.depthMoments;
    }
    return packed;
}
} // namespace irradiance_volume
