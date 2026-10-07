#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/ToneMapping.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("ToneMapping");

namespace {

bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool insideUnitRange(const glm::vec3& color) {
    return glm::all(glm::greaterThanEqual(color, glm::vec3(0.0f))) &&
           glm::all(glm::lessThanEqual(color, glm::vec3(1.0f)));
}

void testAgxCurve(TonemapOperator tonemapOperator) {
    CHECK_MESSAGE(glm::all(glm::lessThan(toneMap(glm::vec3(0.0f), tonemapOperator), glm::vec3(0.001f))),
                  "AgX maps black to black");
    const float middleGrey = toneMap(glm::vec3(0.18f), tonemapOperator).g;
    CHECK_MESSAGE((middleGrey > 0.05f && middleGrey < 0.35f), "AgX maps middle grey to a mid display value");
    float previous = -1.0f;
    bool monotonic = true;
    for (float exposureStops = -12.0f; exposureStops <= 8.0f; exposureStops += 0.25f) {
        const float mapped = relativeLuminance(toneMap(glm::vec3(std::exp2(exposureStops)), tonemapOperator));
        monotonic = monotonic && mapped >= previous - 1e-5f;
        previous = mapped;
    }
    CHECK_MESSAGE(monotonic, "AgX is monotonic over a grey ramp");
    CHECK_MESSAGE(previous > 0.9f, "AgX approaches white for very bright input");
    const std::array saturated{glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(0.0f, 50.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, 1000.0f), glm::vec3(1e6f, 1e6f, 0.0f)};
    for (const glm::vec3& color : saturated) {
        CHECK_MESSAGE(insideUnitRange(toneMap(color, tonemapOperator)),
                      "AgX keeps saturated input in the display range");
    }
    const glm::vec3 brightRed = toneMap(glm::vec3(64.0f, 0.5f, 0.5f), tonemapOperator);
    CHECK_MESSAGE((brightRed.g > 0.2f && brightRed.b > 0.2f), "AgX desaturates very bright highlights toward white");
}

TEST_CASE("ToneMapping: tone mapping") {
    testAgxCurve(TonemapOperator::AgX);
    testAgxCurve(TonemapOperator::AgXPunchy);
    const glm::vec3 base = toneMap(glm::vec3(0.6f, 0.2f, 0.1f), TonemapOperator::AgX);
    const glm::vec3 punchy = toneMap(glm::vec3(0.6f, 0.2f, 0.1f), TonemapOperator::AgXPunchy);
    CHECK_MESSAGE(punchy.r - punchy.b > base.r - base.b, "the punchy look adds saturation");
    CHECK_MESSAGE(near(toneMap(glm::vec3(1.0f), TonemapOperator::Aces).r, 0.8038f, 0.001f),
                  "ACES remains available as the legacy operator");
}

TEST_CASE("ToneMapping: histogram") {
    const ExposureSettings settings;
    CHECK_MESSAGE(luminanceHistogramBin(0.0f, settings.minimumLogLuminance, settings.maximumLogLuminance) == 0,
                  "black lands in the first bin");
    CHECK_MESSAGE(luminanceHistogramBin(1e6f, settings.minimumLogLuminance, settings.maximumLogLuminance) == 255,
                  "luminance above the range lands in the last bin");
    const uint32_t middle = luminanceHistogramBin(1.0f, settings.minimumLogLuminance, settings.maximumLogLuminance);
    CHECK_MESSAGE(middle == 160, "log2 luminance 0 maps proportionally across -10..+6 EV");

    std::vector<uint32_t> empty(luminanceHistogramBinCount, 0u);
    CHECK_MESSAGE(!histogramAverageLogLuminance(empty, settings).has_value(), "an empty histogram has no average");

    std::vector<uint32_t> uniform(luminanceHistogramBinCount, 0u);
    uniform[middle] = 1000;
    const std::optional<float> single = histogramAverageLogLuminance(uniform, settings);
    CHECK_MESSAGE((single && near(*single, 1.0f / 32.0f, 1e-4f)),
                  "a single-bin histogram averages to that bin's center");

    // Half the pixels are black and 5% are blown out: the 50%-95% window ignores both.
    std::vector<uint32_t> mixed(luminanceHistogramBinCount, 0u);
    mixed[0] = 500;
    mixed[middle] = 450;
    mixed[255] = 50;
    const std::optional<float> clamped = histogramAverageLogLuminance(mixed, settings);
    CHECK_MESSAGE((clamped && near(*clamped, *single, 1e-4f)),
                  "percentile clamping rejects dark and blown-out outliers");

    // Window straddles two bins: 20% of the window from the high bin.
    std::vector<uint32_t> split(luminanceHistogramBinCount, 0u);
    split[100] = 860;
    split[200] = 140;
    const std::optional<float> blended = histogramAverageLogLuminance(split, settings);
    const float lowCenter = -10.0f + 100.5f / 256.0f * 16.0f;
    const float highCenter = -10.0f + 200.5f / 256.0f * 16.0f;
    const float expected = (360.0f * lowCenter + 90.0f * highCenter) / 450.0f;
    CHECK_MESSAGE((blended && near(*blended, expected, 1e-3f)), "partial bins contribute their share of the window");
}

TEST_CASE("ToneMapping: exposure") {
    ExposureSettings settings;
    CHECK_MESSAGE(near(autoExposureTarget(std::log2(0.18f), 0.0f), 1.0f, 1e-5f),
                  "a middle-grey average needs unit exposure");
    CHECK_MESSAGE(near(autoExposureTarget(std::log2(0.18f), 1.0f), 2.0f, 1e-5f), "compensation is applied in stops");
    settings.manualEv = -1.0f;
    settings.compensationEv = 0.5f;
    CHECK_MESSAGE(near(manualExposure(settings), std::exp2(-0.5f), 1e-5f),
                  "manual exposure combines EV and compensation");

    const ExposureSettings defaults;
    const float twoSteps = adaptExposure(adaptExposure(1.0f, 8.0f, 0.008f, defaults), 8.0f, 0.008f, defaults);
    const float oneStep = adaptExposure(1.0f, 8.0f, 0.016f, defaults);
    CHECK_MESSAGE(near(twoSteps, oneStep, 1e-4f), "brightening adaptation is frame-rate independent");
    const float darkTwo = adaptExposure(adaptExposure(4.0f, 0.5f, 0.008f, defaults), 0.5f, 0.008f, defaults);
    CHECK_MESSAGE(near(darkTwo, adaptExposure(4.0f, 0.5f, 0.016f, defaults), 1e-4f),
                  "darkening adaptation is frame-rate independent");
    CHECK_MESSAGE((oneStep > 1.0f && oneStep < 8.0f), "adaptation moves partway toward the target");
    CHECK_MESSAGE(near(adaptExposure(1.0f, 8.0f, 0.0f, defaults), 1.0f, 1e-6f),
                  "zero time keeps the previous exposure");
    CHECK_MESSAGE(near(adaptExposure(1.0f, 8.0f, 100.0f, defaults), 8.0f, 1e-3f),
                  "long intervals converge on the target");
    CHECK_MESSAGE(near(adaptExposure(0.0f, 3.0f, 0.016f, defaults), 3.0f, 1e-6f),
                  "invalid history snaps to the target");
    const float fasterDown = std::log2(4.0f / adaptExposure(4.0f, 1.0f, 0.1f, defaults));
    const float slowerUp = std::log2(adaptExposure(1.0f, 4.0f, 0.1f, defaults) / 1.0f);
    CHECK_MESSAGE(fasterDown > slowerUp, "separate speeds apply when exposure rises or falls");
}

TEST_CASE("ToneMapping: settings") {
    RenderSettings requested;
    requested.bloom = BloomQuality::Downsampled;
    requested.bloomIntensity = 0.08f;
    requested.bloomFilterRadius = 1.5f;
    requested.tonemap = TonemapOperator::AgXPunchy;
    requested.exposure.mode = ExposureMode::Manual;
    requested.exposure.manualEv = 1.5f;
    requested.exposure.compensationEv = -0.5f;
    requested.exposure.lowPercentile = 0.4f;
    requested.exposure.highPercentile = 0.9f;
    requested.budgets.exposure.gpuMilliseconds = 0.2;
    std::string encoded;
    RenderSettings decoded;
    CHECK_MESSAGE((serializeRenderSettings(requested, encoded) && deserializeRenderSettings(encoded, decoded)),
                  "exposure settings round trip");
    CHECK_MESSAGE((decoded.bloomIntensity == requested.bloomIntensity &&
                   decoded.bloomFilterRadius == requested.bloomFilterRadius && decoded.tonemap == requested.tonemap &&
                   decoded.exposure.mode == ExposureMode::Manual && decoded.exposure.manualEv == 1.5f &&
                   decoded.exposure.compensationEv == -0.5f && decoded.exposure.lowPercentile == 0.4f &&
                   decoded.exposure.highPercentile == 0.9f && decoded.budgets.exposure.gpuMilliseconds == 0.2),
                  "round-tripped settings match");

    RenderSettings legacy;
    CHECK_MESSAGE(
        (static_cast<bool>(deserializeRenderSettings(
            R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":1,"environment":0,"localLights":1})",
            legacy))),
        "version-1 documents without exposure fields load");
    const RenderSettings defaults;
    CHECK_MESSAGE((legacy.tonemap == TonemapOperator::AgX && legacy.exposure.mode == ExposureMode::Auto &&
                   legacy.bloomIntensity == defaults.bloomIntensity &&
                   legacy.exposure.highPercentile == defaults.exposure.highPercentile),
                  "missing fields use defaults");

    const auto rejects = [](const char* document) {
        RenderSettings ignored;
        return !deserializeRenderSettings(document, ignored);
    };
    const std::string prefix =
        R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":1,"environment":0,"localLights":1,)";
    CHECK_MESSAGE(rejects((prefix + R"("tonemap":7})").c_str()), "unknown tone-mapping operators are rejected");
    CHECK_MESSAGE(rejects((prefix + R"("exposure":{"mode":4}})").c_str()), "unknown exposure modes are rejected");
    CHECK_MESSAGE(rejects((prefix + R"("exposure":{"lowPercentile":0.9,"highPercentile":0.5}})").c_str()),
                  "inverted percentile windows are rejected");
    CHECK_MESSAGE(rejects((prefix + R"("exposure":{"minimumLogLuminance":2,"maximumLogLuminance":2}})").c_str()),
                  "empty luminance ranges are rejected");
    CHECK_MESSAGE(rejects((prefix + R"("exposure":{"adaptationSpeedUp":0}})").c_str()),
                  "zero adaptation speed is rejected");
    CHECK_MESSAGE(rejects((prefix + R"("bloomIntensity":2})").c_str()), "bloom intensity above one is rejected");
}
} // namespace

TEST_SUITE_END();
