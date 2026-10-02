#include "RenderConfiguration.hpp"

#include "MotionBlur.hpp"
#include "DepthOfField.hpp"

#include <cmath>
#include <limits>
#include <utility>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;

// Beyond this, simplified levels would be visibly wrong at any distance.
constexpr float maximumLodErrorThresholdPixels = 64.0f;
// Colour-grading ranges, documented on the settings in Rendering.hpp.
constexpr float minimumVignetteSmoothness = 0.01f;
constexpr float maximumFilmGrainIntensity = 0.5f;
constexpr float minimumFilmGrainSize = 1.0f;
constexpr float maximumFilmGrainSize = 8.0f;

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

Json upscalerDocument(const UpscalerSettings& upscaler) {
    return {{"mode", upscaler.mode}, {"quality", upscaler.quality}, {"sharpness", upscaler.sharpness}};
}

// Optional: documents written before upscalers keep the Off default, so
// antialiasing 2 with dynamic resolution still means TAA upsampling.
bool readUpscaler(const Json& document, UpscalerSettings& output) {
    const auto found = document.find("upscaler");
    if (found == document.end()) {
        return true;
    }
    if (!found->is_object() ||
        (found->contains("mode") && !readEnum(*found, "mode", UpscalerMode::Fsr1, output.mode)) ||
        (found->contains("quality") && !readEnum(*found, "quality", UpscalerQuality::Performance, output.quality))) {
        return false;
    }
    if (const auto sharpness = found->find("sharpness"); sharpness != found->end()) {
        if (!sharpness->is_number()) {
            return false;
        }
        output.sharpness = sharpness->get<float>();
    }
    return std::isfinite(output.sharpness) && output.sharpness >= 0.0f && output.sharpness <= 1.0f;
}

// FSR 1 falls back to TAA upsampling where temporal anti-aliasing exists and
// to native resolution otherwise. TAA upsampling reconstructs with TAA, so it
// selects TAA, or falls back to native resolution without it.
void validateUpscaler(ValidatedRenderSettings& result, const RenderCapabilities& capabilities) {
    UpscalerSettings& upscaler = result.settings.upscaler;
    if (upscaler.mode == UpscalerMode::Fsr1 && !capabilities.spatialUpscaling) {
        if (capabilities.temporalAntialiasing) {
            upscaler.mode = UpscalerMode::Taau;
            result.fallbacks.emplace_back("FSR 1 upscaling is unsupported; using TAA upsampling");
        } else {
            upscaler.mode = UpscalerMode::Off;
            result.fallbacks.emplace_back("FSR 1 upscaling is unsupported; rendering at native resolution");
        }
    }
    if (upscaler.mode != UpscalerMode::Taau) {
        return;
    }
    if (!capabilities.temporalAntialiasing) {
        upscaler.mode = UpscalerMode::Off;
        result.fallbacks.emplace_back("TAA upsampling needs temporal antialiasing; rendering at native resolution");
    } else if (result.settings.antialiasing != AntialiasingMode::Taa) {
        result.settings.antialiasing = AntialiasingMode::Taa;
        result.fallbacks.emplace_back("TAA upsampling reconstructs with temporal antialiasing, which was enabled");
    }
}

Json depthOfFieldDocument(const DepthOfFieldSettings& depthOfField) {
    return {{"quality", depthOfField.quality},
            {"focusDistance", depthOfField.focusDistance},
            {"fStop", depthOfField.fStop},
            {"sensorHeightMillimeters", depthOfField.sensorHeightMillimeters},
            {"maximumCocRadiusPixels", depthOfField.maximumCocRadiusPixels}};
}

// The whole object is optional, and so is each field: documents written before
// depth of field keep it Off with default lens settings.
bool readDepthOfField(const Json& document, DepthOfFieldSettings& output) {
    const auto found = document.find("depthOfField");
    if (found == document.end()) {
        return true;
    }
    if (!found->is_object() ||
        (found->contains("quality") && !readEnum(*found, "quality", DepthOfFieldQuality::High, output.quality))) {
        return false;
    }
    const DepthOfFieldSettings defaults;
    output.focusDistance = found->value("focusDistance", defaults.focusDistance);
    output.fStop = found->value("fStop", defaults.fStop);
    output.sensorHeightMillimeters = found->value("sensorHeightMillimeters", defaults.sensorHeightMillimeters);
    output.maximumCocRadiusPixels = found->value("maximumCocRadiusPixels", defaults.maximumCocRadiusPixels);
    return depth_of_field::validSettings(output);
}

Json colorGradeDocument(const RenderSettings& settings) {
    return {{"colorLut",
             {{"enabled", settings.colorLut.enabled},
              {"path", settings.colorLut.path},
              {"strength", settings.colorLut.strength}}},
            {"vignette",
             {{"enabled", settings.vignette.enabled},
              {"intensity", settings.vignette.intensity},
              {"smoothness", settings.vignette.smoothness},
              {"roundness", settings.vignette.roundness}}},
            {"filmGrain",
             {{"enabled", settings.filmGrain.enabled},
              {"intensity", settings.filmGrain.intensity},
              {"size", settings.filmGrain.size}}}};
}

// Each colour-grading object and each of its fields is optional; missing ones
// keep their defaults (every effect Off). Throws on mistyped fields.
bool readColorGrade(const Json& document, RenderSettings& output) {
    const auto object = [&](const char* name) -> const Json* {
        const auto found = document.find(name);
        return found == document.end() ? nullptr : &*found;
    };
    if (const Json* lut = object("colorLut")) {
        if (!lut->is_object()) {
            return false;
        }
        output.colorLut.enabled = lut->value("enabled", output.colorLut.enabled);
        output.colorLut.path = lut->value("path", output.colorLut.path);
        output.colorLut.strength = lut->value("strength", output.colorLut.strength);
    }
    if (const Json* vignette = object("vignette")) {
        if (!vignette->is_object()) {
            return false;
        }
        output.vignette.enabled = vignette->value("enabled", output.vignette.enabled);
        output.vignette.intensity = vignette->value("intensity", output.vignette.intensity);
        output.vignette.smoothness = vignette->value("smoothness", output.vignette.smoothness);
        output.vignette.roundness = vignette->value("roundness", output.vignette.roundness);
    }
    if (const Json* grain = object("filmGrain")) {
        if (!grain->is_object()) {
            return false;
        }
        output.filmGrain.enabled = grain->value("enabled", output.filmGrain.enabled);
        output.filmGrain.intensity = grain->value("intensity", output.filmGrain.intensity);
        output.filmGrain.size = grain->value("size", output.filmGrain.size);
    }
    return true;
}

bool inRange(float value, float minimum, float maximum) {
    return value >= minimum && value <= maximum;
}

bool validColorGrade(const RenderSettings& settings) {
    return inRange(settings.colorLut.strength, 0.0f, 1.0f) && inRange(settings.vignette.intensity, 0.0f, 1.0f) &&
           inRange(settings.vignette.smoothness, minimumVignetteSmoothness, 1.0f) &&
           inRange(settings.vignette.roundness, 0.0f, 1.0f) &&
           inRange(settings.filmGrain.intensity, 0.0f, maximumFilmGrainIntensity) &&
           inRange(settings.filmGrain.size, minimumFilmGrainSize, maximumFilmGrainSize);
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
                           {"upscaler", upscalerDocument(settings.upscaler)},
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
                           {"localShadows", settings.localShadows},
                           {"reflectionProbes", settings.reflectionProbes},
                           {"reflectionProbeBlending", settings.reflectionProbeBlending},
                           {"indirectDiffuse", settings.indirectDiffuse},
                           {"preferGpuDrivenSubmission", settings.preferGpuDrivenSubmission},
                           {"depthPrepass", settings.depthPrepass},
                           {"lodErrorThresholdPixels", settings.lodErrorThresholdPixels},
                           {"cascadeSplitLambda", settings.cascadeSplitLambda},
                           {"cascadeBlendFraction", settings.cascadeBlendFraction},
                           {"visualizeShadowCascades", settings.visualizeShadowCascades},
                           {"textureStreamingBytesPerFrame", settings.textureStreamingBytesPerFrame},
                           {"dynamicResolution",
                            {{"enabled", settings.dynamicResolution.enabled},
                             {"minimumScale", settings.dynamicResolution.minimumScale},
                             {"maximumScale", settings.dynamicResolution.maximumScale},
                             {"targetGpuMilliseconds", settings.dynamicResolution.targetGpuMilliseconds},
                             {"adjustmentRate", settings.dynamicResolution.adjustmentRate},
                             {"quantization", settings.dynamicResolution.quantization}}},
                           {"ambientOcclusion", settings.ambientOcclusion},
                           {"reflections", settings.reflections},
                           {"volumetricFog", settings.volumetricFog},
                           {"motionBlur", settings.motionBlur},
                           {"motionBlurShutterFraction", settings.motionBlurShutterFraction},
                           {"motionBlurMaximumRadiusPixels", settings.motionBlurMaximumRadiusPixels},
                           {"depthOfField", depthOfFieldDocument(settings.depthOfField)},
                           {"colorGrade", colorGradeDocument(settings)},
                           {"decals", settings.decals},
                           {"particles", settings.particles},
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
                             {"exposure", budget(settings.budgets.exposure)},
                             {"depthPrepass", budget(settings.budgets.depthPrepass)},
                             {"depthPyramid", budget(settings.budgets.depthPyramid)},
                             {"lightClusters", budget(settings.budgets.lightClusters)},
                             {"environment", budget(settings.budgets.environment)},
                             {"localShadows", budget(settings.budgets.localShadows)},
                             {"shadowCulling", budget(settings.budgets.shadowCulling)},
                             {"reflectionProbeClusters", budget(settings.budgets.reflectionProbeClusters)},
                             {"volumetricFog", budget(settings.budgets.volumetricFog)},
                             {"irradianceVolume", budget(settings.budgets.irradianceVolume)},
                             {"motionBlur", budget(settings.budgets.motionBlur)},
                             {"depthOfField", budget(settings.budgets.depthOfField)},
                             {"colorGrade", budget(settings.budgets.colorGrade)},
                             {"spatialUpscale", budget(settings.budgets.spatialUpscale)},
                             {"decalClusters", budget(settings.budgets.decalClusters)},
                             {"particleSimulation", budget(settings.budgets.particleSimulation)},
                             {"particleDraw", budget(settings.budgets.particleDraw)}}}};
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
        if (!readUpscaler(document, loaded.upscaler)) {
            return {.error = "Render settings contain invalid upscaler settings"};
        }
        loaded.environmentBackground = document.value("environmentBackground", true);
        if (const auto maximumLights = document.find("maximumLocalLights"); maximumLights != document.end()) {
            if (!maximumLights->is_number_unsigned() || maximumLights->get<uint64_t>() == 0 ||
                maximumLights->get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
                return {.error = "Render settings contain an invalid local-light limit"};
            }
            loaded.maximumLocalLights = maximumLights->get<uint32_t>();
        }
        // Documents written before local-light shadows keep the default.
        if (document.contains("localShadows") &&
            !readEnum(document, "localShadows", LocalShadowQuality::High, loaded.localShadows)) {
            return {.error = "Render settings contain an unknown local-shadow quality"};
        }
        loaded.reflectionProbes = document.value("reflectionProbes", true);
        if (document.contains("reflectionProbeBlending") &&
            !readEnum(document, "reflectionProbeBlending", ReflectionProbeBlending::PerPixel,
                      loaded.reflectionProbeBlending)) {
            return {.error = "Render settings contain an unknown reflection-probe blending mode"};
        }
        if (document.contains("indirectDiffuse") &&
            !readEnum(document, "indirectDiffuse", IndirectDiffuseMode::ProbeVolume, loaded.indirectDiffuse)) {
            return {.error = "Render settings contain an unknown indirect-diffuse mode"};
        }
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
        if (const auto streaming = document.find("textureStreamingBytesPerFrame"); streaming != document.end()) {
            if (!streaming->is_number_unsigned() || streaming->get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
                return {.error = "Render settings contain an invalid texture-streaming budget"};
            }
            loaded.textureStreamingBytesPerFrame = streaming->get<uint32_t>();
        }
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
        if (document.contains("volumetricFog") &&
            !readEnum(document, "volumetricFog", VolumetricFogQuality::High, loaded.volumetricFog)) {
            return {.error = "Render settings contain an unknown volumetric-fog quality"};
        }
        // Documents written before motion blur keep its defaults (Off).
        if (document.contains("motionBlur") &&
            !readEnum(document, "motionBlur", MotionBlurQuality::High, loaded.motionBlur)) {
            return {.error = "Render settings contain an unknown motion-blur quality"};
        }
        loaded.motionBlurShutterFraction =
            document.value("motionBlurShutterFraction", loaded.motionBlurShutterFraction);
        loaded.motionBlurMaximumRadiusPixels =
            document.value("motionBlurMaximumRadiusPixels", loaded.motionBlurMaximumRadiusPixels);
        if (!motion_blur::validParameters(loaded.motionBlurShutterFraction, loaded.motionBlurMaximumRadiusPixels)) {
            return {.error = "Render settings contain out-of-range motion-blur parameters"};
        }
        if (!readDepthOfField(document, loaded.depthOfField)) {
            return {.error = "Render settings contain invalid depth of field settings"};
        }
        if (const auto colorGrade = document.find("colorGrade"); colorGrade != document.end()) {
            if (!colorGrade->is_object() || !readColorGrade(*colorGrade, loaded)) {
                return {.error = "Render settings contain invalid colour-grading settings"};
            }
        }
        if (!validColorGrade(loaded)) {
            return {.error = "Render settings contain out-of-range colour-grading parameters"};
        }
        // Documents written before clustered decals keep the default.
        if (document.contains("decals") && !readEnum(document, "decals", DecalQuality::On, loaded.decals)) {
            return {.error = "Render settings contain an unknown decal quality"};
        }
        // Documents written before GPU particles keep the default.
        if (document.contains("particles") &&
            !readEnum(document, "particles", ParticleQuality::High, loaded.particles)) {
            return {.error = "Render settings contain an unknown particle quality"};
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
                !readOptionalBudget(*budgets, "exposure", loaded.budgets.exposure) ||
                !readOptionalBudget(*budgets, "depthPrepass", loaded.budgets.depthPrepass) ||
                !readOptionalBudget(*budgets, "depthPyramid", loaded.budgets.depthPyramid) ||
                !readOptionalBudget(*budgets, "lightClusters", loaded.budgets.lightClusters) ||
                !readOptionalBudget(*budgets, "environment", loaded.budgets.environment) ||
                !readOptionalBudget(*budgets, "localShadows", loaded.budgets.localShadows) ||
                !readOptionalBudget(*budgets, "shadowCulling", loaded.budgets.shadowCulling) ||
                !readOptionalBudget(*budgets, "reflectionProbeClusters", loaded.budgets.reflectionProbeClusters) ||
                !readOptionalBudget(*budgets, "volumetricFog", loaded.budgets.volumetricFog) ||
                !readOptionalBudget(*budgets, "irradianceVolume", loaded.budgets.irradianceVolume) ||
                !readOptionalBudget(*budgets, "motionBlur", loaded.budgets.motionBlur) ||
                !readOptionalBudget(*budgets, "depthOfField", loaded.budgets.depthOfField) ||
                !readOptionalBudget(*budgets, "colorGrade", loaded.budgets.colorGrade) ||
                !readOptionalBudget(*budgets, "spatialUpscale", loaded.budgets.spatialUpscale) ||
                !readOptionalBudget(*budgets, "decalClusters", loaded.budgets.decalClusters) ||
                !readOptionalBudget(*budgets, "particleSimulation", loaded.budgets.particleSimulation) ||
                !readOptionalBudget(*budgets, "particleDraw", loaded.budgets.particleDraw)) {
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
        settings = std::move(loaded);
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
    if (result.settings.reflectionProbes &&
        result.settings.reflectionProbeBlending == ReflectionProbeBlending::PerPixel &&
        !capabilities.perPixelReflectionProbes) {
        result.settings.reflectionProbeBlending = ReflectionProbeBlending::CameraOnly;
        result.fallbacks.emplace_back(
            "Per-pixel reflection probe blending is unsupported; blending the dominant probe at the camera");
    }
    if (result.settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume && !capabilities.irradianceVolumes) {
        result.settings.indirectDiffuse = IndirectDiffuseMode::Environment;
        result.fallbacks.emplace_back("Irradiance volumes are unsupported; using environment indirect diffuse");
    }
    if (result.settings.localLights != LocalLightingQuality::Disabled && capabilities.maximumLocalLights == 0) {
        result.settings.localLights = LocalLightingQuality::Disabled;
        result.fallbacks.emplace_back("Local lighting is unsupported and was disabled");
    } else if (result.settings.maximumLocalLights > capabilities.maximumLocalLights) {
        result.settings.maximumLocalLights = capabilities.maximumLocalLights;
        result.fallbacks.emplace_back("The local-light limit exceeds the renderer's capacity and was reduced");
    }
    if (result.settings.localShadows != LocalShadowQuality::Off &&
        result.settings.localLights == LocalLightingQuality::Disabled) {
        result.settings.localShadows = LocalShadowQuality::Off;
        result.fallbacks.emplace_back("Local-light shadows need clustered local lighting and were disabled");
    }
    if (result.settings.preferGpuDrivenSubmission && !capabilities.gpuDrivenPath) {
        result.settings.preferGpuDrivenSubmission = false;
        result.fallbacks.emplace_back("GPU-driven submission is unsupported or slower; using compatible submission");
    }
    if (result.settings.antialiasing == AntialiasingMode::Taa && !capabilities.temporalAntialiasing) {
        result.settings.antialiasing = AntialiasingMode::Fxaa;
        result.fallbacks.emplace_back("Temporal antialiasing is unsupported; using FXAA");
    }
    validateUpscaler(result, capabilities);
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
    if (result.settings.volumetricFog != VolumetricFogQuality::Off && !capabilities.volumetricFog) {
        result.settings.volumetricFog = VolumetricFogQuality::Off;
        result.fallbacks.emplace_back("Volumetric fog needs RGBA16F 3D storage images and was disabled");
    }
    if (result.settings.motionBlur != MotionBlurQuality::Off && !capabilities.motionBlur) {
        result.settings.motionBlur = MotionBlurQuality::Off;
        result.fallbacks.emplace_back("Motion blur is unsupported and was disabled");
    }
    if (result.settings.depthOfField.quality != DepthOfFieldQuality::Off && !capabilities.depthOfField) {
        result.settings.depthOfField.quality = DepthOfFieldQuality::Off;
        result.fallbacks.emplace_back("Depth of field needs RGBA16F storage images and was disabled");
    }
    // Documents are range-checked on load; settings set in code are not.
    if (result.settings.depthOfField.quality != DepthOfFieldQuality::Off &&
        !depth_of_field::validSettings(result.settings.depthOfField)) {
        result.settings.depthOfField.quality = DepthOfFieldQuality::Off;
        result.fallbacks.emplace_back("Depth of field lens settings are out of range; depth of field was disabled");
    }
    if (result.settings.decals != DecalQuality::Off && !capabilities.decals) {
        result.settings.decals = DecalQuality::Off;
        result.fallbacks.emplace_back("Clustered decals are unsupported and were disabled");
    }
    if (result.settings.particles != ParticleQuality::Off && !capabilities.particles) {
        result.settings.particles = ParticleQuality::Off;
        result.fallbacks.emplace_back("GPU particles are unsupported and were disabled");
    }
    return result;
}
