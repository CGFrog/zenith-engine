#include "engine/graphics/TemporalQuality.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <iostream>

TEST_SUITE_BEGIN("TemporalQuality");

namespace {

bool approximately(float first, float second, float tolerance = 0.0001f) {
    return std::abs(first - second) <= tolerance;
}

// Reversed-Z projection matching MeshPass.
glm::mat4 sceneProjection(float aspect) {
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 500.0f, 0.1f);
    projection[1][1] *= -1.0f;
    return projection;
}

glm::vec2 internalPixel(const glm::vec4& clip, glm::vec2 internalSize) {
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * internalSize;
}

DynamicResolutionSettings fixedScale(float scale) {
    return {.enabled = true,
            .minimumScale = scale,
            .maximumScale = scale,
            .targetGpuMilliseconds = 10.0f,
            .adjustmentRate = 0.1f,
            .quantization = 8};
}

TEST_CASE("TemporalQuality: history lifecycle") {
    TemporalQualityController controller;
    DynamicResolutionSettings native;
    const TemporalFrameState first = controller.beginFrame(1920, 1080, 12.0, native, true, false);
    const TemporalFrameState second = controller.beginFrame(1920, 1080, 12.0, native, true, false);
    CHECK_MESSAGE((first.historyInvalidated && !first.historyValid),
                  "the first temporal frame rejects undefined history");
    CHECK_MESSAGE((second.historyValid && !second.historyInvalidated), "stable subsequent frames consume history");
    CHECK_MESSAGE(first.jitterPixels != second.jitterPixels,
                  "temporal jitter advances through a deterministic sequence");
    CHECK_MESSAGE((std::abs(first.jitterNdc.x) <= 1.0f / 1920.0f && std::abs(first.jitterNdc.y) <= 1.0f / 1080.0f),
                  "native-resolution jitter remains inside one output pixel");

    const TemporalFrameState cut = controller.beginFrame(1920, 1080, 12.0, native, true, true);
    CHECK_MESSAGE((cut.cameraCut && cut.historyInvalidated && !cut.historyValid),
                  "camera cuts invalidate temporal history for one frame");
    CHECK_MESSAGE(controller.beginFrame(1920, 1080, 12.0, native, true, false).historyValid,
                  "history becomes valid after the post-cut frame is recorded");

    const TemporalFrameState resized = controller.beginFrame(1280, 720, 12.0, native, true, false);
    CHECK_MESSAGE((resized.historyInvalidated && !resized.historyValid), "an output resize invalidates history");

    controller.beginFrame(1280, 720, 12.0, native, false, false);
    const TemporalFrameState reenabled = controller.beginFrame(1280, 720, 12.0, native, true, false);
    CHECK_MESSAGE(!reenabled.historyValid, "re-enabling temporal anti-aliasing starts from fresh history");

    controller.invalidateHistory();
    CHECK_MESSAGE(controller.beginFrame(1280, 720, 8.0, native, true, false).historyInvalidated,
                  "explicit resize or device-loss invalidation is observable");
}

TEST_CASE("TemporalQuality: dynamic resolution keeps history") {
    TemporalQualityController controller;
    const DynamicResolutionSettings dynamic{.enabled = true,
                                            .minimumScale = 0.5f,
                                            .maximumScale = 1.0f,
                                            .targetGpuMilliseconds = 10.0f,
                                            .adjustmentRate = 0.1f,
                                            .quantization = 16};
    TemporalFrameState previous = controller.beginFrame(1920, 1080, 20.0, dynamic, true, false);
    bool scaleChanged = false;
    bool historyKeptAcrossChange = true;
    for (int frame = 0; frame < 24; ++frame) {
        const TemporalFrameState state = controller.beginFrame(1920, 1080, 20.0, dynamic, true, false);
        if (state.internalWidth != previous.internalWidth) {
            scaleChanged = true;
            historyKeptAcrossChange = historyKeptAcrossChange && state.historyValid && !state.historyInvalidated;
        }
        previous = state;
    }
    CHECK_MESSAGE((scaleChanged && previous.resolutionScale < 1.0f),
                  "sustained over-budget GPU time lowers internal resolution slowly");
    CHECK_MESSAGE((previous.internalWidth % 16 == 0 && previous.internalHeight % 16 == 0),
                  "dynamic extents are quantized for stable resource reuse");
    CHECK_MESSAGE(historyKeptAcrossChange, "output-resolution history survives internal resolution changes");
}

TEST_CASE("TemporalQuality: jitter spans one internal pixel") {
    for (const float scale : {1.0f, 0.75f, 0.5f}) {
        TemporalQualityController controller;
        glm::vec2 minimum(1.0f);
        glm::vec2 maximum(-1.0f);
        bool consistent = true;
        bool projectionMatches = true;
        for (int frame = 0; frame < 16; ++frame) {
            const TemporalFrameState state = controller.beginFrame(1600, 900, 0.0, fixedScale(scale), true, false);
            const glm::vec2 internalSize(static_cast<float>(state.internalWidth),
                                         static_cast<float>(state.internalHeight));
            minimum = glm::min(minimum, state.jitterPixels);
            maximum = glm::max(maximum, state.jitterPixels);
            consistent = consistent && approximately(state.jitterNdc.x * internalSize.x * 0.5f, state.jitterPixels.x) &&
                         approximately(state.jitterNdc.y * internalSize.y * 0.5f, state.jitterPixels.y);

            // A jittered projection moves content by -jitterPixels internal
            // pixels, so internal texel c samples the scene at c + jitter.
            const glm::mat4 projection = sceneProjection(internalSize.x / internalSize.y);
            const glm::vec4 point(0.7f, -0.4f, -6.0f, 1.0f);
            const glm::vec2 unjittered = internalPixel(projection * point, internalSize);
            const glm::vec2 jittered =
                internalPixel(applyProjectionJitter(projection, state.jitterNdc) * point, internalSize);
            projectionMatches = projectionMatches &&
                                approximately(jittered.x, unjittered.x - state.jitterPixels.x, 0.001f) &&
                                approximately(jittered.y, unjittered.y - state.jitterPixels.y, 0.001f);
        }
        CHECK_MESSAGE((minimum.x >= -0.5f && minimum.y >= -0.5f && maximum.x <= 0.5f && maximum.y <= 0.5f),
                      "jitter stays within one internal pixel");
        CHECK_MESSAGE((maximum.x - minimum.x > 0.75f && maximum.y - minimum.y > 0.75f),
                      "the jitter sequence covers the internal pixel footprint");
        CHECK_MESSAGE(consistent, "NDC jitter is measured in internal-resolution pixels");
        CHECK_MESSAGE(projectionMatches, "projection jitter shifts content by the reported internal-pixel offset");
    }
}

TEST_CASE("TemporalQuality: unjittered motion") {
    const float aspect = 16.0f / 9.0f;
    const glm::mat4 projection = sceneProjection(aspect);
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 1.5f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec2 previousJitter(0.0007f, -0.0004f);
    const glm::vec2 currentJitter(-0.0003f, 0.0009f);
    const glm::mat4 previousViewProjection = applyProjectionJitter(projection, previousJitter) * view;
    const glm::mat4 currentViewProjection = applyProjectionJitter(projection, currentJitter) * view;
    const glm::vec4 jitter(currentJitter, previousJitter);

    const glm::vec4 staticPoint(0.3f, 0.2f, -1.0f, 1.0f);
    const glm::vec2 staticMotion =
        unjitteredMotionUv(currentViewProjection * staticPoint, previousViewProjection * staticPoint, jitter);
    CHECK_MESSAGE((approximately(staticMotion.x, 0.0f, 0.000001f) && approximately(staticMotion.y, 0.0f, 0.000001f)),
                  "a static scene has zero motion despite per-frame jitter");

    const glm::vec4 movedPoint = staticPoint + glm::vec4(0.25f, 0.0f, 0.0f, 0.0f);
    const glm::vec2 motion =
        unjitteredMotionUv(currentViewProjection * movedPoint, previousViewProjection * staticPoint, jitter);
    const glm::vec4 currentClip = projection * view * movedPoint;
    const glm::vec4 previousClip = projection * view * staticPoint;
    const glm::vec2 expected =
        (glm::vec2(currentClip) / currentClip.w - glm::vec2(previousClip) / previousClip.w) * 0.5f;
    CHECK_MESSAGE((approximately(motion.x, expected.x, 0.00001f) && approximately(motion.y, expected.y, 0.00001f)),
                  "object motion equals the unjittered screen displacement");
}

TEST_CASE("TemporalQuality: neighborhood clip") {
    const std::array<glm::vec3, 9> samples{glm::vec3(0.40f), glm::vec3(0.45f), glm::vec3(0.50f),
                                           glm::vec3(0.55f), glm::vec3(0.60f), glm::vec3(0.50f),
                                           glm::vec3(0.48f), glm::vec3(0.52f), glm::vec3(0.50f)};
    const TemporalNeighborhood neighborhood = measureTemporalNeighborhood(samples);
    CHECK_MESSAGE((approximately(neighborhood.mean.x, 0.5f, 0.001f) && approximately(neighborhood.mean.y, 0.0f) &&
                   approximately(neighborhood.mean.z, 0.0f)),
                  "neighborhood statistics are measured in YCoCg");

    const glm::vec3 inRange(0.52f);
    CHECK_MESSAGE(clipTemporalHistory(inRange, neighborhood, 1.0f) == inRange,
                  "history inside the variance box is kept");

    const glm::vec3 outlier(2.0f, 0.1f, 0.5f);
    const glm::vec3 clipped = clipTemporalHistory(outlier, neighborhood, 1.0f);
    const glm::vec3 clippedOffset = glm::abs(rgbToYCoCg(clipped) - neighborhood.mean);
    const glm::vec3 limit = glm::max(neighborhood.standardDeviation, glm::vec3(0.0001f)) + 0.0001f;
    CHECK_MESSAGE((clippedOffset.x <= limit.x && clippedOffset.y <= limit.y && clippedOffset.z <= limit.z),
                  "outlying history is clipped onto the variance box");
    CHECK_MESSAGE(glm::length(rgbToYCoCg(clipped) - neighborhood.mean) <
                      glm::length(rgbToYCoCg(outlier) - neighborhood.mean),
                  "clipping moves history toward the neighborhood mean");

    const glm::vec3 color(0.8f, 0.3f, 0.1f);
    const glm::vec3 roundTrip = yCoCgToRgb(rgbToYCoCg(color));
    CHECK_MESSAGE((approximately(roundTrip.x, color.x) && approximately(roundTrip.y, color.y) &&
                   approximately(roundTrip.z, color.z)),
                  "YCoCg conversion round-trips");
    CHECK_MESSAGE(temporalClipGamma(0.0f) > temporalClipGamma(16.0f), "the clip box tightens under motion");
}

TEST_CASE("TemporalQuality: resolve blend") {
    CHECK_MESSAGE((approximately(temporalReconstructionWeight(glm::vec2(0.0f)), 1.0f) &&
                   temporalReconstructionWeight(glm::vec2(0.5f)) < temporalReconstructionWeight(glm::vec2(0.25f))),
                  "reconstruction favors samples near the output pixel center");

    const std::array<glm::vec3, 9> flat{glm::vec3(0.4f), glm::vec3(0.6f), glm::vec3(0.5f),
                                        glm::vec3(0.5f), glm::vec3(0.5f), glm::vec3(0.5f),
                                        glm::vec3(0.5f), glm::vec3(0.5f), glm::vec3(0.5f)};
    const TemporalNeighborhood neighborhood = measureTemporalNeighborhood(flat);
    const TemporalResolveSample stable =
        resolveTemporalSample({.current = glm::vec3(0.5f), .history = glm::vec3(0.55f), .neighborhood = neighborhood});
    CHECK_MESSAGE((stable.historyWeight > 0.85f && stable.color.x > 0.5f && stable.color.x < 0.55f),
                  "stable history dominates accumulation");

    const TemporalResolveSample reactive = resolveTemporalSample(
        {.current = {1.0f, 0.2f, 0.1f}, .history = glm::vec3(0.1f), .neighborhood = neighborhood, .reactive = 1.0f});
    CHECK_MESSAGE((reactive.historyWeight == 0.0f && reactive.color == glm::vec3(1.0f, 0.2f, 0.1f)),
                  "reactive emissive or transparent pixels favor the current frame");

    const TemporalResolveSample rejected = resolveTemporalSample({.current = glm::vec3(0.2f),
                                                                  .history = glm::vec3(0.8f),
                                                                  .neighborhood = neighborhood,
                                                                  .historyAvailable = false});
    CHECK_MESSAGE((rejected.historyWeight == 0.0f && rejected.color == glm::vec3(0.2f)),
                  "disoccluded or off-screen pixels reject stale history completely");

    CHECK_MESSAGE(temporalHistoryWeight(1.0f, 0.0f, 0.0f) < temporalHistoryWeight(0.3f, 0.0f, 0.0f),
                  "well-placed internal samples contribute more to the output pixel");
    CHECK_MESSAGE(temporalHistoryWeight(1.0f, 40.0f, 0.0f) < temporalHistoryWeight(1.0f, 0.0f, 0.0f),
                  "fast motion shortens the accumulation window");

    const TemporalResolveSample bright = resolveTemporalSample(
        {.current = glm::vec3(20.0f),
         .history = glm::vec3(0.5f),
         .neighborhood = measureTemporalNeighborhood(std::array{glm::vec3(0.5f), glm::vec3(20.0f)}),
         .sampleConfidence = 1.0f});
    const float linearBlend = 0.5f + (20.0f - 0.5f) * (1.0f - temporalHistoryWeight(1.0f, 0.0f, 0.0f));
    CHECK_MESSAGE(bright.color.x < linearBlend, "luma weighting suppresses single bright samples");
}
} // namespace

TEST_SUITE_END();
