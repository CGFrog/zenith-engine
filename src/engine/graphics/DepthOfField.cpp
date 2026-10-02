#include "DepthOfField.hpp"

#include <algorithm>
#include <cmath>

namespace depth_of_field {
namespace {
bool inRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

float smoothStep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

bool validSettings(const DepthOfFieldSettings& settings) {
    return inRange(settings.focusDistance, minimumFocusDistance, maximumFocusDistance) &&
           inRange(settings.fStop, minimumFStop, maximumFStop) &&
           inRange(settings.sensorHeightMillimeters, minimumSensorHeightMillimeters, maximumSensorHeightMillimeters) &&
           inRange(settings.maximumCocRadiusPixels, minimumCocRadiusPixels, maximumCocRadiusPixels);
}

CameraTerms cameraFromProjection(const glm::mat4& projection) {
    // projection[1][1] is cot(fov / 2), negated by the Y flip.
    return {.verticalFovRadians = 2.0f * std::atan(1.0f / std::abs(projection[1][1])),
            .depthScale = projection[2][2],
            .depthBias = projection[3][2]};
}

float viewDistanceFromDepth(float depth, const CameraTerms& camera) {
    return camera.depthBias / (depth + camera.depthScale);
}

float focalLengthMeters(float verticalFovRadians, float sensorHeightMeters) {
    return sensorHeightMeters / (2.0f * std::tan(0.5f * verticalFovRadians));
}

float CircleOfConfusion::radiusPixels(float viewDistance) const {
    const float unclamped = scale * (1.0f - focusDistance / std::max(viewDistance, 1e-6f));
    return std::clamp(unclamped, -maximumRadius, maximumRadius);
}

CircleOfConfusion circleOfConfusion(const DepthOfFieldSettings& settings, float verticalFovRadians,
                                    float imageHeightPixels) {
    const float sensorHeight = settings.sensorHeightMillimeters * 0.001f;
    const float focalLength = focalLengthMeters(verticalFovRadians, sensorHeight);
    const float focusDistance = std::max(settings.focusDistance, focalLength * 1.01f);
    const float apertureDiameter = focalLength / settings.fStop;
    const float pixelsPerMeter = imageHeightPixels / sensorHeight;
    return {.scale = 0.5f * apertureDiameter * focalLength / (focusDistance - focalLength) * pixelsPerMeter,
            .focusDistance = focusDistance,
            .maximumRadius = settings.maximumCocRadiusPixels};
}

glm::uvec2 gatherExtent(glm::uvec2 fullExtent) {
    return (fullExtent + 1u) / 2u;
}

float gatherRingSpacing(DepthOfFieldQuality quality) {
    switch (quality) {
    case DepthOfFieldQuality::Low:
        return 1.0f;
    case DepthOfFieldQuality::High:
        return 0.5f;
    case DepthOfFieldQuality::Off:
        break;
    }
    return 0.0f;
}

uint32_t gatherSampleCount(float maximumRadiusHalfPixels, float spacing) {
    if (!(spacing > 0.0f)) {
        return 0;
    }
    uint32_t count = 0;
    for (float radius = spacing; radius < maximumRadiusHalfPixels && count < maximumGatherSamples;
         radius += spacing / radius) {
        ++count;
    }
    return count;
}

float compositeBlend(float effectiveRadiusPixels) {
    return smoothStep(0.5f, 2.0f, effectiveRadiusPixels);
}

} // namespace depth_of_field
