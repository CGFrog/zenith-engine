#include "MotionBlur.hpp"

#include <algorithm>
#include <cmath>

namespace motion_blur {

uint32_t sampleCount(MotionBlurQuality quality) {
    switch (quality) {
    case MotionBlurQuality::Low:
        return 8;
    case MotionBlurQuality::High:
        return 16;
    case MotionBlurQuality::Off:
        break;
    }
    return 0;
}

bool validParameters(float shutterFraction, float maximumRadius) {
    return shutterFraction >= minimumShutterFraction && shutterFraction <= maximumShutterFraction &&
           maximumRadius >= minimumRadiusPixels && maximumRadius <= maximumRadiusPixels;
}

float velocityScale(float shutterFraction, float frameSeconds) {
    const float shutter = std::clamp(shutterFraction, minimumShutterFraction, maximumShutterFraction);
    if (!(frameSeconds > 0.0f)) {
        return shutter;
    }
    const float frame = std::clamp(frameSeconds, minimumFrameSeconds, maximumFrameSeconds);
    return shutter * referenceFrameSeconds / frame;
}

float regionRadiusPixels(float maximumRadiusOutputPixels, uint32_t regionWidth, uint32_t outputWidth) {
    const float radius = std::clamp(maximumRadiusOutputPixels, minimumRadiusPixels, maximumRadiusPixels);
    if (outputWidth == 0) {
        return radius;
    }
    const float scale = static_cast<float>(regionWidth) / static_cast<float>(outputWidth);
    return std::max(radius * std::min(scale, 1.0f), minimumRadiusPixels);
}

uint32_t tileSize(float radiusPixels) {
    const uint32_t radius = static_cast<uint32_t>(std::ceil(std::max(radiusPixels, 1.0f)));
    return (radius + tileGranularity - 1) / tileGranularity * tileGranularity;
}

glm::uvec2 tileCount(glm::uvec2 regionPixels, uint32_t size) {
    const uint32_t edge = std::max(size, 1u);
    return (regionPixels + glm::uvec2(edge - 1)) / edge;
}

glm::vec2 blurVelocity(const glm::vec2& motionUv, const glm::uvec2& regionPixels, float scale, float radiusPixels) {
    const glm::vec2 velocity = 0.5f * motionUv * glm::vec2(regionPixels) * scale;
    const float length = glm::length(velocity);
    return length > radiusPixels ? velocity * (radiusPixels / length) : velocity;
}

FramePlan planFrame(const RenderSettings& settings, glm::uvec2 regionPixels, uint32_t outputWidth, float frameSeconds) {
    FramePlan plan;
    plan.samples = sampleCount(settings.motionBlur);
    plan.velocityScale = velocityScale(settings.motionBlurShutterFraction, frameSeconds);
    plan.radiusPixels = regionRadiusPixels(settings.motionBlurMaximumRadiusPixels, regionPixels.x, outputWidth);
    plan.tileSize = tileSize(plan.radiusPixels);
    plan.tiles = tileCount(regionPixels, plan.tileSize);
    return plan;
}

} // namespace motion_blur
