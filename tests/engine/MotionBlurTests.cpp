#include "engine/graphics/MotionBlur.hpp"
#include "engine/graphics/RenderConfiguration.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

TEST_SUITE_BEGIN("MotionBlur");

namespace {

bool approximately(float actual, float expected, float tolerance = 1e-5f) {
    return std::abs(actual - expected) <= tolerance * std::max(std::abs(expected), 1.0f);
}

TEST_CASE("MotionBlur: sample counts") {
    CHECK_MESSAGE(motion_blur::sampleCount(MotionBlurQuality::Off) == 0, "Off gathers no samples");
    CHECK_MESSAGE(motion_blur::sampleCount(MotionBlurQuality::Low) == 8, "Low gathers 8 samples");
    CHECK_MESSAGE(motion_blur::sampleCount(MotionBlurQuality::High) == 16, "High gathers 16 samples");
}

TEST_CASE("MotionBlur: velocity scale") {
    constexpr float reference = motion_blur::referenceFrameSeconds;
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(0.5f, reference), 0.5f),
                  "at the reference frame rate the scale is the shutter fraction");
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(0.5f, reference * 0.5f), 1.0f),
                  "at twice the frame rate each frame's shorter motion is doubled");
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(0.5f, reference * 2.0f), 0.25f),
                  "at half the frame rate each frame's longer motion is halved");
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(0.5f, 0.0f), 0.5f),
                  "without a previous frame the reference frame time is assumed");
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(1.0f, 10.0f), reference / motion_blur::maximumFrameSeconds),
                  "hitches are clamped to the longest frame time");
    CHECK_MESSAGE(approximately(motion_blur::velocityScale(1.0f, 1e-6f), reference / motion_blur::minimumFrameSeconds),
                  "very short frames are clamped to the shortest frame time");
    CHECK_MESSAGE(motion_blur::velocityScale(0.0f, reference) == 0.0f, "a closed shutter does not blur");

    // A point crossing the screen at a constant on-screen speed blurs by the
    // same length at any frame rate.
    const glm::uvec2 region{1920, 1080};
    const float screenWidthsPerSecond = 0.5f;
    for (const float frameSeconds : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
        const glm::vec2 motionUv{screenWidthsPerSecond * frameSeconds, 0.0f};
        const glm::vec2 velocity = motion_blur::blurVelocity(
            motionUv, region, motion_blur::velocityScale(0.5f, frameSeconds), motion_blur::maximumRadiusPixels);
        // Half of 0.5 * 1920 px/s over a 0.5 * (1 / 60) s exposure.
        CHECK_MESSAGE(approximately(velocity.x, 0.5f * 960.0f * 0.5f / 60.0f, 1e-4f),
                      "blur length is independent of the frame rate");
    }
}

TEST_CASE("MotionBlur: blur velocity") {
    const glm::uvec2 region{1000, 500};
    const glm::vec2 velocity = motion_blur::blurVelocity({0.01f, -0.02f}, region, 1.0f, 64.0f);
    CHECK_MESSAGE((approximately(velocity.x, 5.0f) && approximately(velocity.y, -5.0f)),
                  "the blur velocity is half the motion in region pixels");
    const glm::vec2 clamped = motion_blur::blurVelocity({0.3f, 0.4f}, region, 1.0f, 10.0f);
    CHECK_MESSAGE(approximately(glm::length(clamped), 10.0f), "long blurs are clamped to the radius");
    CHECK_MESSAGE(approximately(clamped.x / clamped.y, (0.3f * 1000.0f) / (0.4f * 500.0f)),
                  "clamping keeps the blur direction");
    CHECK_MESSAGE(motion_blur::blurVelocity({0.0f, 0.0f}, region, 1.0f, 10.0f) == glm::vec2(0.0f),
                  "static pixels do not blur");
}

TEST_CASE("MotionBlur: tiles") {
    CHECK_MESSAGE(motion_blur::tileSize(1.0f) == 8, "small radii use the smallest tile");
    CHECK_MESSAGE(motion_blur::tileSize(8.0f) == 8, "a radius equal to the granularity fits one tile");
    CHECK_MESSAGE(motion_blur::tileSize(8.5f) == 16, "tiles round up to cover the radius");
    CHECK_MESSAGE((motion_blur::tileSize(32.0f) == 32 && motion_blur::tileSize(64.0f) == 64),
                  "multiples of the granularity are kept");
    for (float radius = 1.0f; radius <= motion_blur::maximumRadiusPixels; radius += 0.75f) {
        const uint32_t size = motion_blur::tileSize(radius);
        CHECK_MESSAGE((static_cast<float>(size) >= radius && size % motion_blur::tileGranularity == 0),
                      "every tile is at least as large as the radius");
    }
    CHECK_MESSAGE(motion_blur::tileCount({1920, 1080}, 32) == glm::uvec2(60, 34), "partial tiles are counted");
    CHECK_MESSAGE(motion_blur::tileCount({1920, 1080}, 8) == glm::uvec2(240, 135), "exact tiles are counted");
}

TEST_CASE("MotionBlur: region radius") {
    CHECK_MESSAGE(approximately(motion_blur::regionRadiusPixels(32.0f, 1920, 1920), 32.0f),
                  "an output-sized region keeps the radius");
    CHECK_MESSAGE(approximately(motion_blur::regionRadiusPixels(32.0f, 960, 1920), 16.0f),
                  "a half-resolution region halves the radius");
    CHECK_MESSAGE(approximately(motion_blur::regionRadiusPixels(1.0f, 100, 1920), motion_blur::minimumRadiusPixels),
                  "the radius never falls below one pixel");
    CHECK_MESSAGE(approximately(motion_blur::regionRadiusPixels(500.0f, 1920, 1920), motion_blur::maximumRadiusPixels),
                  "the radius is clamped to its maximum");
}

TEST_CASE("MotionBlur: frame plan") {
    RenderSettings settings;
    settings.motionBlur = MotionBlurQuality::High;
    settings.motionBlurShutterFraction = 0.5f;
    settings.motionBlurMaximumRadiusPixels = 20.0f;
    const motion_blur::FramePlan plan =
        motion_blur::planFrame(settings, {1280, 720}, 1280, motion_blur::referenceFrameSeconds);
    CHECK_MESSAGE((plan.samples == 16 && approximately(plan.velocityScale, 0.5f) &&
                   approximately(plan.radiusPixels, 20.0f) && plan.tileSize == 24 && plan.tiles == glm::uvec2(54, 30)),
                  "a full-resolution frame plan derives samples, scale, radius and tiles");
    const motion_blur::FramePlan scaled =
        motion_blur::planFrame(settings, {640, 360}, 1280, motion_blur::referenceFrameSeconds);
    CHECK_MESSAGE(
        (approximately(scaled.radiusPixels, 10.0f) && scaled.tileSize == 16 && scaled.tiles == glm::uvec2(40, 23)),
        "a dynamic-resolution region scales the radius and tiles");
    settings.motionBlur = MotionBlurQuality::Off;
    CHECK_MESSAGE(motion_blur::planFrame(settings, {1280, 720}, 1280, 0.0f).samples == 0, "Off plans no samples");
}

TEST_CASE("MotionBlur: settings") {
    RenderSettings settings;
    CHECK_MESSAGE(settings.motionBlur == MotionBlurQuality::Off, "motion blur is off by default");
    CHECK_MESSAGE(
        motion_blur::validParameters(settings.motionBlurShutterFraction, settings.motionBlurMaximumRadiusPixels),
        "the default parameters are valid");
    settings.motionBlur = MotionBlurQuality::Low;
    settings.motionBlurShutterFraction = 0.75f;
    settings.motionBlurMaximumRadiusPixels = 40.0f;
    settings.budgets.motionBlur = {0.05, 0.6};
    std::string document;
    RenderSettings restored;
    CHECK_MESSAGE(
        (serializeRenderSettings(settings, document).success && deserializeRenderSettings(document, restored).success &&
         restored.motionBlur == MotionBlurQuality::Low && restored.motionBlurShutterFraction == 0.75f &&
         restored.motionBlurMaximumRadiusPixels == 40.0f && restored.budgets.motionBlur.gpuMilliseconds == 0.6),
        "the motion blur settings and budget round trip through the render-settings document");

    constexpr const char* legacyPrefix = R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,)"
                                         R"("environment":0,"localLights":1)";
    RenderSettings legacy;
    CHECK_MESSAGE((deserializeRenderSettings(std::string(legacyPrefix) + "}", legacy).success &&
                   legacy.motionBlur == MotionBlurQuality::Off && legacy.motionBlurShutterFraction == 0.5f &&
                   legacy.motionBlurMaximumRadiusPixels == 32.0f),
                  "documents without motion blur load with it off and default parameters");
    RenderSettings invalid;
    CHECK_MESSAGE(!deserializeRenderSettings(std::string(legacyPrefix) + R"(,"motionBlur":3})", invalid).success,
                  "unknown motion blur qualities are rejected");
    CHECK_MESSAGE(
        !deserializeRenderSettings(std::string(legacyPrefix) + R"(,"motionBlurShutterFraction":1.5})", invalid).success,
        "shutter fractions above one are rejected");
    CHECK_MESSAGE(
        !deserializeRenderSettings(std::string(legacyPrefix) + R"(,"motionBlurMaximumRadiusPixels":0.5})", invalid)
             .success,
        "radii below one pixel are rejected");
    CHECK_MESSAGE(
        !deserializeRenderSettings(std::string(legacyPrefix) + R"(,"motionBlurMaximumRadiusPixels":100})", invalid)
             .success,
        "radii above the maximum are rejected");

    RenderCapabilities capabilities;
    CHECK_MESSAGE(validateRenderSettings(settings, capabilities).settings.motionBlur == MotionBlurQuality::Low,
                  "supported devices keep motion blur");
    capabilities.motionBlur = false;
    const ValidatedRenderSettings validated = validateRenderSettings(settings, capabilities);
    CHECK_MESSAGE((validated.settings.motionBlur == MotionBlurQuality::Off && !validated.fallbacks.empty()),
                  "unsupported devices fall back to motion blur off with an explanation");
}
} // namespace

TEST_SUITE_END();
