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
                           {"budgets",
                            {{"shadow", budget(settings.budgets.shadow)},
                             {"opaque", budget(settings.budgets.opaque)},
                             {"blended", budget(settings.budgets.blended)},
                             {"bloom", budget(settings.budgets.bloom)},
                             {"ui", budget(settings.budgets.ui)},
                             {"upload", budget(settings.budgets.upload)}}}};
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
        if (!readEnum(document, "antialiasing", AntialiasingMode::Fxaa, loaded.antialiasing) ||
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
        if (const auto budgets = document.find("budgets"); budgets != document.end()) {
            if (!budgets->is_object() || !readBudget(*budgets, "shadow", loaded.budgets.shadow) ||
                !readBudget(*budgets, "opaque", loaded.budgets.opaque) ||
                !readBudget(*budgets, "blended", loaded.budgets.blended) ||
                !readBudget(*budgets, "bloom", loaded.budgets.bloom) ||
                !readBudget(*budgets, "ui", loaded.budgets.ui) ||
                !readBudget(*budgets, "upload", loaded.budgets.upload)) {
                return {.error = "Render settings contain invalid pass budgets"};
            }
        }
        if (loaded.cascadeSplitLambda < 0.0f || loaded.cascadeSplitLambda > 1.0f ||
            loaded.cascadeBlendFraction < 0.0f || loaded.cascadeBlendFraction > 0.5f) {
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
    return result;
}
