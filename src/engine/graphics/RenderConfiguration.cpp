#include "RenderConfiguration.hpp"

#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;

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
} // namespace

RenderConfigurationResult serializeRenderSettings(const RenderSettings& settings, std::string& output) {
    const Json document = {{"version", 1},
                           {"antialiasing", settings.antialiasing},
                           {"shadows", settings.shadows},
                           {"lighting", settings.lighting},
                           {"bloom", settings.bloom},
                           {"environment", settings.environment},
                           {"localLights", settings.localLights},
                           {"reflectionProbes", settings.reflectionProbes},
                           {"preferGpuDrivenSubmission", settings.preferGpuDrivenSubmission},
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
                             {"reflections", budget(settings.budgets.reflections)}}}};
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
            !readEnum(document, "localLights", LocalLightingQuality::Tiled64, loaded.localLights)) {
            return {.error = "Render settings contain an unknown quality value"};
        }
        loaded.reflectionProbes = document.value("reflectionProbes", true);
        loaded.preferGpuDrivenSubmission = document.value("preferGpuDrivenSubmission", false);
        loaded.cascadeSplitLambda = document.value("cascadeSplitLambda", 0.65f);
        loaded.cascadeBlendFraction = document.value("cascadeBlendFraction", 0.10f);
        loaded.visualizeShadowCascades = document.value("visualizeShadowCascades", false);
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
                !readOptionalBudget(*budgets, "reflections", loaded.budgets.reflections)) {
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
    const uint32_t requestedLights = result.settings.localLights == LocalLightingQuality::Tiled64 ? 64u : 32u;
    if (result.settings.localLights != LocalLightingQuality::Disabled && capabilities.maximumLocalLightsPerTile == 0) {
        result.settings.localLights = LocalLightingQuality::Disabled;
        result.fallbacks.emplace_back("Local lighting is unsupported and was disabled");
    } else if (requestedLights > capabilities.maximumLocalLightsPerTile) {
        result.settings.localLights = LocalLightingQuality::Tiled32;
        result.fallbacks.emplace_back("64-light tiles are unsupported; using 32-light tiles");
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
    return result;
}
