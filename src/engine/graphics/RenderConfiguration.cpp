#include "RenderConfiguration.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

#include <limits>

namespace {
using Json = nlohmann::json;

// Beyond this, simplified levels would be visibly wrong at any distance.
constexpr float maximumLodErrorThresholdPixels = 64.0f;

template <typename Enum> bool readEnum(const Json& document, const char* name, Enum maximum, Enum& value) {
    const auto found = document.find(name);
    if (found == document.end() || !found->is_number_unsigned()) {
        return false;
    }
    const uint32_t raw = found->get<uint32_t>();
    if (raw > static_cast<uint32_t>(maximum)) {
        return false;
    }
    value = static_cast<Enum>(raw);
    return true;
}

Json budget(const RenderPassBudget& value) {
    return {{"cpuMilliseconds", value.cpuMilliseconds}, {"gpuMilliseconds", value.gpuMilliseconds}};
}

bool readBudget(const Json& document, const char* name, RenderPassBudget& output) {
    const auto found = document.find(name);
    if (found == document.end() || !found->is_object()) {
        return false;
    }
    output.cpuMilliseconds = found->value("cpuMilliseconds", -1.0);
    output.gpuMilliseconds = found->value("gpuMilliseconds", -1.0);
    return output.cpuMilliseconds >= 0.0 && output.gpuMilliseconds >= 0.0;
}

bool readOptionalBudget(const Json& document, const char* name, RenderPassBudget& output) {
    return !document.contains(name) || readBudget(document, name, output);
}

Json exposureDocument(const ExposureSettings& exposure) {
    return {{"mode", exposure.mode},
            {"compensationEv", exposure.compensationEv},
            {"manualEv", exposure.manualEv},
            {"minimumLogLuminance", exposure.minimumLogLuminance},
            {"maximumLogLuminance", exposure.maximumLogLuminance},
            {"adaptationSpeedUp", exposure.adaptationSpeedUp},
            {"adaptationSpeedDown", exposure.adaptationSpeedDown},
            {"lowPercentile", exposure.lowPercentile},
            {"highPercentile", exposure.highPercentile}};
}

// Missing fields keep their defaults so older version-1 documents still load.
bool readExposure(const Json& document, ExposureSettings& output) {
    const auto found = document.find("exposure");
    if (found == document.end()) {
        return true;
    }
    if (!found->is_object() ||
        (found->contains("mode") && !readEnum(*found, "mode", ExposureMode::Manual, output.mode))) {
        return false;
    }
    const ExposureSettings defaults;
    output.compensationEv = found->value("compensationEv", defaults.compensationEv);
    output.manualEv = found->value("manualEv", defaults.manualEv);
    output.minimumLogLuminance = found->value("minimumLogLuminance", defaults.minimumLogLuminance);
    output.maximumLogLuminance = found->value("maximumLogLuminance", defaults.maximumLogLuminance);
    output.adaptationSpeedUp = found->value("adaptationSpeedUp", defaults.adaptationSpeedUp);
    output.adaptationSpeedDown = found->value("adaptationSpeedDown", defaults.adaptationSpeedDown);
    output.lowPercentile = found->value("lowPercentile", defaults.lowPercentile);
    output.highPercentile = found->value("highPercentile", defaults.highPercentile);
    return true;
}

bool validExposure(const ExposureSettings& exposure) {
    return std::isfinite(exposure.compensationEv) && std::abs(exposure.compensationEv) <= 16.0f &&
           std::isfinite(exposure.manualEv) && std::abs(exposure.manualEv) <= 32.0f &&
           exposure.minimumLogLuminance >= -32.0f && exposure.maximumLogLuminance <= 32.0f &&
           exposure.maximumLogLuminance - exposure.minimumLogLuminance >= 1.0f && exposure.adaptationSpeedUp > 0.0f &&
           exposure.adaptationSpeedDown > 0.0f && exposure.lowPercentile >= 0.0f && exposure.highPercentile <= 1.0f &&
           exposure.lowPercentile < exposure.highPercentile;
}

bool validBloom(const RenderSettings& settings) {
    return settings.bloomIntensity >= 0.0f && settings.bloomIntensity <= 1.0f && settings.bloomFilterRadius > 0.0f &&
           settings.bloomFilterRadius <= 4.0f;
}

// Accepts the retired tiled modes (1: 32 lights, 2: 64 lights) as Clustered;
// the light cap then comes from maximumLocalLights or its default.
bool readLocalLighting(const Json& document, LocalLightingQuality& value) {
    constexpr uint32_t legacyTiled64 = 2;
    const auto found = document.find("localLights");
    if (found == document.end() || !found->is_number_unsigned() || found->get<uint32_t>() > legacyTiled64) {
        return false;
    }
    value = found->get<uint32_t>() == 0 ? LocalLightingQuality::Disabled : LocalLightingQuality::Clustered;
    return true;
}
} // namespace

RenderConfigurationResult serializeRenderSettings(const RenderSettings& settings, std::string& output) {
    const Json document = {{"version", 1},
                           {"antialiasing", settings.antialiasing},
                           {"shadows", settings.shadows},
                           {"lighting", settings.lighting},
                           {"bloom", settings.bloom},
                           {"bloomIntensity", settings.bloomIntensity},
                           {"bloomFilterRadius", settings.bloomFilterRadius},
                           {"tonemap", settings.tonemap},
                           {"exposure", exposureDocument(settings.exposure)},
                           {"environment", settings.environment},
                           {"environmentBackground", settings.environmentBackground},
                           {"localLights", settings.localLights},
                           {"maximumLocalLights", settings.maximumLocalLights},
                           {"reflectionProbes", settings.reflectionProbes},
                           {"preferGpuDrivenSubmission", settings.preferGpuDrivenSubmission},
                           {"depthPrepass", settings.depthPrepass},
                           {"lodErrorThresholdPixels", settings.lodErrorThresholdPixels},
                           {"cascadeSplitLambda", settings.cascadeSplitLambda},
                           {"cascadeBlendFraction", settings.cascadeBlendFraction},
                           {"visualizeShadowCascades", settings.visualizeShadowCascades},
                           {"dynamicResolution",
                            {{"enabled", settings.dynamicResolution.enabled},
                             {"minimumScale", settings.dynamicResolution.minimumScale},
                             {"maximumScale", settings.dynamicResolution.maximumScale},
                             {"targetGpuMilliseconds", settings.dynamicResolution.targetGpuMilliseconds},
                             {"adjustmentRate", settings.dynamicResolution.adjustmentRate},
                             {"quantization", settings.dynamicResolution.quantization}}},
                           {"ambientOcclusion", settings.ambientOcclusion},
                           {"reflections", settings.reflections},
                           {"budgets",
                            {{"shadow", budget(settings.budgets.shadow)},
                             {"opaque", budget(settings.budgets.opaque)},
                             {"blended", budget(settings.budgets.blended)},
                             {"bloom", budget(settings.budgets.bloom)},
                             {"ui", budget(settings.budgets.ui)},
                             {"upload", budget(settings.budgets.upload)},
                             {"temporalResolve", budget(settings.budgets.temporalResolve)},
                             {"ambientOcclusion", budget(settings.budgets.ambientOcclusion)},
                             {"reflections", budget(settings.budgets.reflections)},
                             {"exposure", budget(settings.budgets.exposure)}}}};
    output = document.dump(2);
    return {.success = true};
}

RenderConfigurationResult deserializeRenderSettings(std::string_view input, RenderSettings& settings) {
    try {
        const Json document = Json::parse(input);
        if (!document.is_object() || document.value("version", 0) != 1) {
            return {.error = "Unsupported render-settings version"};
        }
        RenderSettings loaded;
        if (!readEnum(document, "antialiasing", AntialiasingMode::Taa, loaded.antialiasing) ||
            !readEnum(document, "shadows", ShadowQuality::CascadedHigh, loaded.shadows) ||
            !readEnum(document, "lighting", LightingQuality::PhysicallyBased, loaded.lighting) ||
            !readEnum(document, "bloom", BloomQuality::Downsampled, loaded.bloom) ||
            !readEnum(document, "environment", EnvironmentQuality::ImageBasedHigh, loaded.environment) ||
            !readLocalLighting(document, loaded.localLights)) {
            return {.error = "Render settings contain an unknown quality value"};
        }
        loaded.environmentBackground = document.value("environmentBackground", true);
        if (const auto maximumLights = document.find("maximumLocalLights"); maximumLights != document.end()) {
            if (!maximumLights->is_number_unsigned() || maximumLights->get<uint64_t>() == 0 ||
                maximumLights->get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
                return {.error = "Render settings contain an invalid local-light limit"};
            }
            loaded.maximumLocalLights = maximumLights->get<uint32_t>();
        }
        loaded.reflectionProbes = document.value("reflectionProbes", true);
        loaded.preferGpuDrivenSubmission = document.value("preferGpuDrivenSubmission", false);
        loaded.depthPrepass = document.value("depthPrepass", true);
        loaded.lodErrorThresholdPixels = document.value("lodErrorThresholdPixels", 1.0f);
        if (!(loaded.lodErrorThresholdPixels >= 0.0f &&
              loaded.lodErrorThresholdPixels <= maximumLodErrorThresholdPixels)) {
            return {.error = "Render settings contain an invalid LOD error threshold"};
        }
        loaded.cascadeSplitLambda = document.value("cascadeSplitLambda", 0.65f);
        loaded.cascadeBlendFraction = document.value("cascadeBlendFraction", 0.10f);
        loaded.visualizeShadowCascades = document.value("visualizeShadowCascades", false);
        loaded.bloomIntensity = document.value("bloomIntensity", loaded.bloomIntensity);
        loaded.bloomFilterRadius = document.value("bloomFilterRadius", loaded.bloomFilterRadius);
        if (document.contains("tonemap") &&
            !readEnum(document, "tonemap", TonemapOperator::AgXPunchy, loaded.tonemap)) {
            return {.error = "Render settings contain an unknown tone-mapping operator"};
        }
        if (!readExposure(document, loaded.exposure)) {
            return {.error = "Render settings contain invalid exposure settings"};
        }
        if (!validExposure(loaded.exposure) || !validBloom(loaded)) {
            return {.error = "Render settings contain out-of-range exposure or bloom parameters"};
        }
        if (const auto dynamicResolution = document.find("dynamicResolution"); dynamicResolution != document.end()) {
            if (!dynamicResolution->is_object()) {
                return {.error = "Render settings contain invalid dynamic-resolution settings"};
            }
            loaded.dynamicResolution.enabled = dynamicResolution->value("enabled", false);
            loaded.dynamicResolution.minimumScale = dynamicResolution->value("minimumScale", 0.67f);
            loaded.dynamicResolution.maximumScale = dynamicResolution->value("maximumScale", 1.0f);
            loaded.dynamicResolution.targetGpuMilliseconds = dynamicResolution->value("targetGpuMilliseconds", 14.0f);
            loaded.dynamicResolution.adjustmentRate = dynamicResolution->value("adjustmentRate", 0.025f);
            loaded.dynamicResolution.quantization = dynamicResolution->value("quantization", 16u);
        }
        if (document.contains("ambientOcclusion") &&
            !readEnum(document, "ambientOcclusion", AmbientOcclusionQuality::FullResolution, loaded.ambientOcclusion)) {
            return {.error = "Render settings contain an unknown ambient-occlusion quality"};
        }
        if (document.contains("reflections") &&
            !readEnum(document, "reflections", ReflectionQuality::ScreenSpaceFullResolution, loaded.reflections)) {
            return {.error = "Render settings contain an unknown reflection quality"};
        }
        if (const auto budgets = document.find("budgets"); budgets != document.end()) {
            if (!budgets->is_object() || !readBudget(*budgets, "shadow", loaded.budgets.shadow) ||
                !readBudget(*budgets, "opaque", loaded.budgets.opaque) ||
                !readBudget(*budgets, "blended", loaded.budgets.blended) ||
                !readBudget(*budgets, "bloom", loaded.budgets.bloom) ||
                !readBudget(*budgets, "ui", loaded.budgets.ui) ||
                !readBudget(*budgets, "upload", loaded.budgets.upload) ||
                !readOptionalBudget(*budgets, "temporalResolve", loaded.budgets.temporalResolve) ||
                !readOptionalBudget(*budgets, "ambientOcclusion", loaded.budgets.ambientOcclusion) ||
                !readOptionalBudget(*budgets, "reflections", loaded.budgets.reflections) ||
                !readOptionalBudget(*budgets, "exposure", loaded.budgets.exposure)) {
                return {.error = "Render settings contain invalid pass budgets"};
            }
        }
        if (loaded.cascadeSplitLambda < 0.0f || loaded.cascadeSplitLambda > 1.0f ||
            loaded.cascadeBlendFraction < 0.0f || loaded.cascadeBlendFraction > 0.5f ||
            loaded.dynamicResolution.minimumScale < 0.25f || loaded.dynamicResolution.maximumScale > 1.0f ||
            loaded.dynamicResolution.minimumScale > loaded.dynamicResolution.maximumScale ||
            loaded.dynamicResolution.targetGpuMilliseconds <= 0.0f || loaded.dynamicResolution.adjustmentRate <= 0.0f ||
            loaded.dynamicResolution.adjustmentRate > 0.25f || loaded.dynamicResolution.quantization == 0 ||
            loaded.dynamicResolution.quantization > 256) {
            return {.error = "Render settings contain invalid cascade parameters"};
        }
        settings = loaded;
        return {.success = true};
    } catch (const Json::exception& error) {
        return {.error = std::string("Could not parse render settings: ") + error.what()};
    }
}

ValidatedRenderSettings validateRenderSettings(const RenderSettings& requested,
                                               const RenderCapabilities& capabilities) {
    ValidatedRenderSettings result{requested};
    if (result.settings.environment != EnvironmentQuality::Hemisphere && !capabilities.imageBasedLighting) {
        result.settings.environment = EnvironmentQuality::Hemisphere;
        result.fallbacks.emplace_back("Image-based lighting is unsupported; using hemisphere lighting");
    }
    if (result.settings.reflectionProbes && !capabilities.reflectionProbes) {
        result.settings.reflectionProbes = false;
        result.fallbacks.emplace_back("Reflection probes are unsupported and were disabled");
    }
    if (result.settings.localLights != LocalLightingQuality::Disabled && capabilities.maximumLocalLights == 0) {
        result.settings.localLights = LocalLightingQuality::Disabled;
        result.fallbacks.emplace_back("Local lighting is unsupported and was disabled");
    } else if (result.settings.maximumLocalLights > capabilities.maximumLocalLights) {
        result.settings.maximumLocalLights = capabilities.maximumLocalLights;
        result.fallbacks.emplace_back("The local-light limit exceeds the renderer's capacity and was reduced");
    }
    if (result.settings.preferGpuDrivenSubmission && !capabilities.gpuDrivenPath) {
        result.settings.preferGpuDrivenSubmission = false;
        result.fallbacks.emplace_back("GPU-driven submission is unsupported or slower; using compatible submission");
    }
    if (result.settings.antialiasing == AntialiasingMode::Taa && !capabilities.temporalAntialiasing) {
        result.settings.antialiasing = AntialiasingMode::Fxaa;
        result.fallbacks.emplace_back("Temporal antialiasing is unsupported; using FXAA");
    }
    if (result.settings.ambientOcclusion != AmbientOcclusionQuality::Off && !capabilities.screenSpaceAmbientOcclusion) {
        result.settings.ambientOcclusion = AmbientOcclusionQuality::Off;
        result.fallbacks.emplace_back("Screen-space ambient occlusion is unsupported and was disabled");
    }
    if (result.settings.reflections != ReflectionQuality::ProbesOnly && !capabilities.screenSpaceReflections) {
        result.settings.reflections = ReflectionQuality::ProbesOnly;
        result.fallbacks.emplace_back("Screen-space reflections are unsupported; using probes and IBL");
    }
    if (result.settings.reflections != ReflectionQuality::ProbesOnly && !result.settings.depthPrepass) {
        result.settings.reflections = ReflectionQuality::ProbesOnly;
        result.fallbacks.emplace_back("Screen-space reflections need the depth prepass; using probes and IBL");
    }
    return result;
}
