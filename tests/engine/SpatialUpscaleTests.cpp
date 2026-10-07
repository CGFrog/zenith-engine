#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/SpatialUpscale.hpp"
#include "engine/graphics/TemporalQuality.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <string>

TEST_SUITE_BEGIN("SpatialUpscale");

namespace {

bool approximately(float first, float second, float tolerance = 0.0001f) {
    return std::abs(first - second) <= tolerance;
}

bool approximately(const glm::vec3& first, const glm::vec3& second, float tolerance = 0.0001f) {
    return approximately(first.x, second.x, tolerance) && approximately(first.y, second.y, tolerance) &&
           approximately(first.z, second.z, tolerance);
}

bool insideUnitRange(const glm::vec3& color) {
    return glm::all(glm::greaterThanEqual(color, glm::vec3(-0.00001f))) &&
           glm::all(glm::lessThanEqual(color, glm::vec3(1.00001f)));
}

// Taps sampled from a function of the input texel offset from f.
template <typename Image> EasuTaps sampleTaps(Image image) {
    return {.b = image(0, -1),
            .c = image(1, -1),
            .e = image(-1, 0),
            .f = image(0, 0),
            .g = image(1, 0),
            .h = image(2, 0),
            .i = image(-1, 1),
            .j = image(0, 1),
            .k = image(1, 1),
            .l = image(2, 1),
            .n = image(0, 2),
            .o = image(1, 2)};
}

TEST_CASE("SpatialUpscale: presets") {
    CHECK_MESSAGE(approximately(upscalerScaleFactor(UpscalerQuality::Quality), 1.5f), "Quality upscales 1.5x per axis");
    CHECK_MESSAGE(approximately(upscalerScaleFactor(UpscalerQuality::Balanced), 1.7f),
                  "Balanced upscales 1.7x per axis");
    CHECK_MESSAGE(approximately(upscalerScaleFactor(UpscalerQuality::Performance), 2.0f),
                  "Performance upscales 2x per axis");
    CHECK_MESSAGE(upscalerRenderScale({.mode = UpscalerMode::Off, .quality = UpscalerQuality::Performance}) == 1.0f,
                  "no upscaler renders at output resolution");
    CHECK_MESSAGE(
        approximately(upscalerRenderScale({.mode = UpscalerMode::Fsr1, .quality = UpscalerQuality::Performance}), 0.5f),
        "FSR at Performance renders at half resolution per axis");
    CHECK_MESSAGE(approximately(upscalerRenderScale({.mode = UpscalerMode::Taau, .quality = UpscalerQuality::Quality}),
                                1.0f / 1.5f),
                  "TAAU uses the same presets");
}

TEST_CASE("SpatialUpscale: dynamic resolution range") {
    RenderSettings settings;
    settings.dynamicResolution = {.enabled = true, .minimumScale = 0.4f, .maximumScale = 1.0f};
    const DynamicResolutionSettings native = upscaledDynamicResolution(settings.dynamicResolution, settings.upscaler);
    CHECK_MESSAGE((native.enabled && native.minimumScale == 0.4f && native.maximumScale == 1.0f),
                  "without an upscaler dynamic resolution is unchanged");

    settings.upscaler = {.mode = UpscalerMode::Fsr1, .quality = UpscalerQuality::Performance};
    settings.dynamicResolution.enabled = false;
    const DynamicResolutionSettings fixed = upscaledDynamicResolution(settings.dynamicResolution, settings.upscaler);
    CHECK_MESSAGE((fixed.enabled && fixed.minimumScale == 0.5f && fixed.maximumScale == 0.5f),
                  "an upscaler without dynamic resolution fixes the preset scale");

    settings.dynamicResolution.enabled = true;
    const DynamicResolutionSettings relative = upscaledDynamicResolution(settings.dynamicResolution, settings.upscaler);
    CHECK_MESSAGE(
        (relative.enabled && approximately(relative.minimumScale, 0.2f) && approximately(relative.maximumScale, 0.5f)),
        "dynamic resolution scales relative to the preset's render scale");
    CHECK_MESSAGE((relative.targetGpuMilliseconds == settings.dynamicResolution.targetGpuMilliseconds &&
                   relative.quantization == settings.dynamicResolution.quantization),
                  "the dynamic-resolution controller parameters are kept");

    // The frame's internal extent follows the preset through the controller.
    settings.dynamicResolution.enabled = false;
    TemporalQualityController controller;
    const TemporalFrameState frame = controller.beginFrame(
        1920, 1080, 10.0, upscaledDynamicResolution(settings.dynamicResolution, settings.upscaler), false, false);
    CHECK_MESSAGE((frame.internalWidth == 960 && frame.internalHeight == 544),
                  "Performance renders 1080p at 960x540 rounded up to the quantization");
}

TEST_CASE("SpatialUpscale: easu constants") {
    const EasuConstants half = easuConstants({960, 540}, {1920, 1080});
    CHECK_MESSAGE((approximately(half.scale.x, 0.5f) && approximately(half.scale.y, 0.5f)),
                  "EASU scale is input over output");
    CHECK_MESSAGE((approximately(half.offset.x, -0.25f) && approximately(half.offset.y, -0.25f)),
                  "EASU maps output pixel centers to input texel space");
    CHECK_MESSAGE((half.inputMaximum == glm::ivec2(959, 539) && half.outputSize == glm::ivec2(1920, 1080)),
                  "EASU clamps taps to the input viewport");
    // Output pixel 1919's center lands half an output pixel inside the last input texel's right edge.
    CHECK_MESSAGE(approximately(1919.0f * half.scale.x + half.offset.x, 959.25f),
                  "the last output pixel maps inside the input");

    const EasuConstants identity = easuConstants({1280, 720}, {1280, 720});
    CHECK_MESSAGE((identity.scale == glm::vec2(1.0f) && identity.offset == glm::vec2(0.0f)),
                  "equal sizes map output pixels onto input texels");

    const EasuConstants degenerate = easuConstants({0, 0}, {0, 0});
    CHECK_MESSAGE((degenerate.inputMaximum == glm::ivec2(0) && degenerate.outputSize == glm::ivec2(1)),
                  "empty extents clamp to one texel");
}

TEST_CASE("SpatialUpscale: easu reconstruction") {
    const glm::vec3 grey(0.42f, 0.31f, 0.77f);
    const EasuTaps flat = sampleTaps([&](int, int) { return grey; });
    CHECK_MESSAGE(approximately(easuReconstruct(flat, {0.3f, 0.7f}), grey), "a flat image reconstructs unchanged");

    const EasuTaps ramp = sampleTaps([](int x, int) { return glm::vec3(0.4f + 0.1f * static_cast<float>(x)); });
    CHECK_MESSAGE(approximately(easuReconstruct(ramp, {0.5f, 0.5f}), glm::vec3(0.45f)),
                  "a linear ramp reconstructs to its midpoint between texel centers");

    const EasuTaps edge = sampleTaps([](int x, int) { return glm::vec3(x >= 1 ? 1.0f : 0.0f); });
    for (float fraction : {0.0f, 0.25f, 0.5f, 0.75f, 0.999f}) {
        const glm::vec3 color = easuReconstruct(edge, {fraction, 0.5f});
        CHECK_MESSAGE(insideUnitRange(color), "EASU does not ring across a hard edge");
    }
    CHECK_MESSAGE((easuReconstruct(edge, {0.25f, 0.5f}).x < 0.25f && easuReconstruct(edge, {0.75f, 0.5f}).x > 0.75f),
                  "EASU keeps a hard edge sharper than bilinear interpolation");
    CHECK_MESSAGE(approximately(easuReconstruct(edge, {0.0f, 0.0f}), glm::vec3(0.0f)),
                  "a sample on a texel center reproduces it");
}

TEST_CASE("SpatialUpscale: rcas") {
    CHECK_MESSAGE((approximately(rcasSharpnessStops(1.0f), 0.0f) && approximately(rcasLobeScale(1.0f), 1.0f)),
                  "full sharpness applies RCAS without attenuation");
    CHECK_MESSAGE((approximately(rcasLobeScale(0.5f), 0.5f) && approximately(rcasLobeScale(0.0f), 0.25f)),
                  "each half unit of sharpness removes one stop");
    CHECK_MESSAGE(approximately(rcasSharpnessStops(0.9f), 0.2f), "the default sharpness is AMD's 0.2 stops");
    CHECK_MESSAGE((approximately(rcasLobeScale(-1.0f), 0.25f) && approximately(rcasLobeScale(2.0f), 1.0f)),
                  "sharpness is clamped to [0, 1]");

    const glm::vec3 grey(0.5f);
    CHECK_MESSAGE(approximately(rcasSharpen(grey, grey, grey, grey, grey, 1.0f), grey),
                  "flat neighborhoods pass through");

    const glm::vec3 dark(0.2f);
    const glm::vec3 bright(0.6f);
    const glm::vec3 peak = rcasSharpen(dark, dark, bright, dark, dark, 1.0f);
    CHECK_MESSAGE((peak.x > bright.x && insideUnitRange(peak)), "RCAS raises local contrast within range");
    const glm::vec3 softer = rcasSharpen(dark, dark, bright, dark, dark, rcasLobeScale(0.0f));
    CHECK_MESSAGE((softer.x > bright.x && softer.x < peak.x), "lower sharpness sharpens less");
    CHECK_MESSAGE(approximately(rcasSharpen(dark, dark, bright, dark, dark, 0.0f), bright),
                  "a zero lobe scale leaves the pixel unchanged");

    // Deterministic pseudo-random neighborhoods, including saturated values.
    uint32_t state = 12345u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    bool allInRange = true;
    for (int sample = 0; sample < 2000; ++sample) {
        glm::vec3 taps[5];
        for (glm::vec3& tap : taps) {
            tap = glm::vec3(next(), next(), next());
            if (sample % 7 == 0) {
                tap = glm::round(tap);
            }
        }
        allInRange &= insideUnitRange(rcasSharpen(taps[0], taps[1], taps[2], taps[3], taps[4], 1.0f));
    }
    CHECK_MESSAGE(allInRange, "RCAS never pushes a channel outside [0, 1]");
}

TEST_CASE("SpatialUpscale: serialization") {
    RenderSettings source;
    source.upscaler = {.mode = UpscalerMode::Fsr1, .quality = UpscalerQuality::Balanced, .sharpness = 0.6f};
    source.budgets.spatialUpscale = {0.0, 0.4};
    std::string encoded;
    RenderSettings decoded;
    CHECK_MESSAGE((serializeRenderSettings(source, encoded) && deserializeRenderSettings(encoded, decoded)),
                  "upscaler settings round-trip");
    CHECK_MESSAGE(
        (decoded.upscaler.mode == UpscalerMode::Fsr1 && decoded.upscaler.quality == UpscalerQuality::Balanced &&
         approximately(decoded.upscaler.sharpness, 0.6f) && decoded.budgets.spatialUpscale.gpuMilliseconds == 0.4),
        "upscaler mode, preset, sharpness, and budget are preserved");

    nlohmann::json legacy = nlohmann::json::parse(encoded);
    legacy.erase("upscaler");
    legacy["antialiasing"] = 2;
    legacy["dynamicResolution"]["enabled"] = true;
    legacy["budgets"].erase("spatialUpscale");
    RenderSettings migrated;
    CHECK_MESSAGE((deserializeRenderSettings(legacy.dump(), migrated) && migrated.upscaler.mode == UpscalerMode::Off &&
                   migrated.antialiasing == AntialiasingMode::Taa && migrated.dynamicResolution.enabled),
                  "documents without an upscaler keep TAA with dynamic resolution");

    const auto rejects = [&](const char* key, nlohmann::json value) {
        nlohmann::json document = nlohmann::json::parse(encoded);
        document["upscaler"][key] = value;
        RenderSettings rejected;
        return !deserializeRenderSettings(document.dump(), rejected);
    };
    CHECK_MESSAGE(rejects("mode", 3), "an unknown upscaler mode is rejected");
    CHECK_MESSAGE(rejects("quality", 3), "an unknown upscaler preset is rejected");
    CHECK_MESSAGE(rejects("sharpness", 1.5), "sharpness above 1 is rejected");
    CHECK_MESSAGE(rejects("sharpness", "sharp"), "non-numeric sharpness is rejected");

    nlohmann::json partial = nlohmann::json::parse(encoded);
    partial["upscaler"] = {{"mode", 1}};
    RenderSettings partialDecoded;
    CHECK_MESSAGE((deserializeRenderSettings(partial.dump(), partialDecoded) &&
                   partialDecoded.upscaler.mode == UpscalerMode::Taau &&
                   partialDecoded.upscaler.quality == UpscalerQuality::Quality &&
                   approximately(partialDecoded.upscaler.sharpness, UpscalerSettings{}.sharpness)),
                  "missing upscaler fields keep their defaults");
}

TEST_CASE("SpatialUpscale: fallbacks") {
    RenderSettings requested;
    requested.antialiasing = AntialiasingMode::Fxaa;
    requested.upscaler.mode = UpscalerMode::Fsr1;
    // Every other requested feature is supported, so only upscaler fallbacks remain.
    RenderCapabilities capabilities;
    capabilities.imageBasedLighting = true;
    capabilities.reflectionProbes = true;
    capabilities.perPixelReflectionProbes = true;
    capabilities.irradianceVolumes = true;
    capabilities.spatialUpscaling = true;
    const ValidatedRenderSettings supported = validateRenderSettings(requested, capabilities);
    CHECK_MESSAGE((supported.settings.upscaler.mode == UpscalerMode::Fsr1 &&
                   supported.settings.antialiasing == AntialiasingMode::Fxaa && supported.fallbacks.empty()),
                  "supported FSR keeps the requested anti-aliasing");

    capabilities.spatialUpscaling = false;
    const ValidatedRenderSettings toTaau = validateRenderSettings(requested, capabilities);
    CHECK_MESSAGE((toTaau.settings.upscaler.mode == UpscalerMode::Taau &&
                   toTaau.settings.antialiasing == AntialiasingMode::Taa && toTaau.fallbacks.size() == 2),
                  "unsupported FSR falls back to TAA upsampling and enables TAA");

    capabilities.temporalAntialiasing = false;
    const ValidatedRenderSettings toNative = validateRenderSettings(requested, capabilities);
    CHECK_MESSAGE((toNative.settings.upscaler.mode == UpscalerMode::Off &&
                   toNative.settings.antialiasing == AntialiasingMode::Fxaa && toNative.fallbacks.size() == 1),
                  "without FSR or TAA the upscaler falls back to native resolution");

    requested.upscaler.mode = UpscalerMode::Taau;
    requested.antialiasing = AntialiasingMode::Taa;
    const ValidatedRenderSettings noTemporal = validateRenderSettings(requested, capabilities);
    CHECK_MESSAGE((noTemporal.settings.upscaler.mode == UpscalerMode::Off &&
                   noTemporal.settings.antialiasing == AntialiasingMode::Fxaa),
                  "TAA upsampling without TAA support renders natively with FXAA");
}
} // namespace

TEST_SUITE_END();
