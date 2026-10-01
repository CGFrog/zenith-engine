#include "engine/graphics/TemporalQuality.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    TemporalQualityController controller;
    DynamicResolutionSettings native;
    const TemporalFrameState first = controller.beginFrame(1920, 1080, 12.0, native, true, false);
    const TemporalFrameState second = controller.beginFrame(1920, 1080, 12.0, native, true, false);
    expect(first.historyInvalidated && !first.historyValid, "the first temporal frame rejects undefined history");
    expect(second.historyValid && !second.historyInvalidated, "stable subsequent frames consume history");
    expect(first.jitterPixels != second.jitterPixels, "temporal jitter advances through a deterministic sequence");
    expect(std::abs(first.jitterNdc.x) <= 1.0f / 1920.0f && std::abs(first.jitterNdc.y) <= 1.0f / 1080.0f,
           "jitter remains inside one output pixel");

    const TemporalFrameState cut = controller.beginFrame(1920, 1080, 12.0, native, true, true);
    expect(cut.cameraCut && cut.historyInvalidated && !cut.historyValid,
           "camera cuts invalidate temporal history for one frame");
    expect(controller.beginFrame(1920, 1080, 12.0, native, true, false).historyValid,
           "history becomes valid after the post-cut frame is recorded");

    DynamicResolutionSettings dynamic{.enabled = true,
                                      .minimumScale = 0.5f,
                                      .maximumScale = 1.0f,
                                      .targetGpuMilliseconds = 10.0f,
                                      .adjustmentRate = 0.1f,
                                      .quantization = 16};
    TemporalFrameState scaled;
    for (int frame = 0; frame < 12; ++frame) {
        scaled = controller.beginFrame(1920, 1080, 20.0, dynamic, true, false);
    }
    expect(scaled.resolutionScale < 1.0f, "sustained over-budget GPU time lowers internal resolution slowly");
    expect(scaled.internalWidth % 16 == 0 && scaled.internalHeight % 16 == 0,
           "dynamic extents are quantized for stable resource reuse");

    const TemporalResolveSample stable = resolveTemporalSample(
        {0.5f, 0.5f, 0.5f}, {2.0f, -1.0f, 0.6f}, {0.4f, 0.4f, 0.4f}, {0.6f, 0.6f, 0.6f}, 0.0f, 0.0f, 0.0f);
    expect(stable.historyWeight > 0.8f && stable.color.x <= 0.6f && stable.color.y >= 0.4f,
           "stable history is neighborhood-clipped before accumulation");
    const TemporalResolveSample reactive =
        resolveTemporalSample({1.0f, 0.2f, 0.1f}, glm::vec3(0.1f), glm::vec3(0.0f), glm::vec3(1.0f), 0.0f, 0.0f, 1.0f);
    expect(reactive.historyWeight == 0.0f && reactive.color == glm::vec3(1.0f, 0.2f, 0.1f),
           "reactive emissive or transparent pixels favor the current frame");
    const TemporalResolveSample disoccluded =
        resolveTemporalSample(glm::vec3(0.2f), glm::vec3(0.8f), glm::vec3(0.0f), glm::vec3(1.0f), 2.0f, 0.1f, 0.0f);
    expect(disoccluded.disoccluded && disoccluded.historyWeight == 0.0f,
           "depth disocclusion rejects stale history completely");

    controller.invalidateHistory();
    expect(controller.beginFrame(1920, 1080, 8.0, dynamic, true, false).historyInvalidated,
           "explicit resize or device-loss invalidation is observable");
    return failures == 0 ? 0 : 1;
}
