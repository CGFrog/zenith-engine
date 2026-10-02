#include "GraphicsSettingsEditor.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace {
// Upper bounds mirrored from deserializeRenderSettings (RenderConfiguration.cpp).
constexpr float maximumLodErrorPixels = 64.0f;
constexpr float maximumCompensationEv = 16.0f;
constexpr float maximumManualEv = 32.0f;
constexpr float logLuminanceLimit = 32.0f;
constexpr float minimumLogLuminanceSpan = 1.0f;
constexpr float minimumPercentileGap = 0.01f;
constexpr float minimumAdaptationSpeed = 0.01f;
constexpr float minimumDynamicScale = 0.25f;
constexpr float maximumAdjustmentRate = 0.25f;
constexpr uint32_t maximumQuantization = 256;
constexpr float minimumBloomRadius = 0.05f;
constexpr float maximumBloomRadius = 4.0f;

struct EnumOption {
    const char* label;
    bool available{true};
};

// Selects one of the options by index; unavailable options are shown disabled.
template <typename Enum, size_t Count>
bool enumCombo(const char* label, Enum& value, const std::array<EnumOption, Count>& options) {
    const size_t current = std::min(static_cast<size_t>(value), Count - 1);
    bool changed = false;
    if (ImGui::BeginCombo(label, options[current].label)) {
        for (size_t index = 0; index < Count; ++index) {
            const ImGuiSelectableFlags flags = options[index].available ? 0 : ImGuiSelectableFlags_Disabled;
            if (ImGui::Selectable(options[index].label, index == current, flags) && index != current) {
                value = static_cast<Enum>(index);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool drawBudget(const char* label, RenderPassBudget& budget) {
    ImGui::PushID(label);
    float milliseconds[2]{static_cast<float>(budget.cpuMilliseconds), static_cast<float>(budget.gpuMilliseconds)};
    const bool changed = ImGui::DragFloat2(label, milliseconds, 0.01f, 0.0f, 100.0f, "%.2f ms");
    if (changed) {
        budget.cpuMilliseconds = milliseconds[0];
        budget.gpuMilliseconds = milliseconds[1];
    }
    ImGui::PopID();
    return changed;
}

bool drawAntialiasingAndResolution(RenderSettings& settings, const RenderCapabilities& capabilities) {
    bool changed =
        enumCombo("Anti-aliasing", settings.antialiasing,
                  std::array<EnumOption, 3>{{{"Off"}, {"FXAA"}, {"TAA", capabilities.temporalAntialiasing}}});
    DynamicResolutionSettings& dynamic = settings.dynamicResolution;
    changed |= ImGui::Checkbox("Dynamic resolution", &dynamic.enabled);
    ImGui::BeginDisabled(!dynamic.enabled);
    changed |= ImGui::SliderFloat("Minimum scale", &dynamic.minimumScale, minimumDynamicScale, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Maximum scale", &dynamic.maximumScale, minimumDynamicScale, 1.0f, "%.2f");
    changed |= ImGui::DragFloat("Target GPU time", &dynamic.targetGpuMilliseconds, 0.05f, 0.1f, 100.0f, "%.2f ms");
    changed |= ImGui::SliderFloat("Adjustment rate", &dynamic.adjustmentRate, 0.001f, maximumAdjustmentRate, "%.3f");
    int quantization = static_cast<int>(dynamic.quantization);
    if (ImGui::SliderInt("Quantization", &quantization, 1, static_cast<int>(maximumQuantization))) {
        dynamic.quantization = static_cast<uint32_t>(quantization);
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool drawShadows(RenderSettings& settings) {
    bool changed =
        enumCombo("Shadows", settings.shadows,
                  std::array<EnumOption, 4>{{{"Off"}, {"Single map"}, {"Cascaded (balanced)"}, {"Cascaded (high)"}}});
    const bool cascaded =
        settings.shadows == ShadowQuality::CascadedBalanced || settings.shadows == ShadowQuality::CascadedHigh;
    ImGui::BeginDisabled(!cascaded);
    changed |= ImGui::SliderFloat("Cascade split lambda", &settings.cascadeSplitLambda, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Cascade blend fraction", &settings.cascadeBlendFraction, 0.0f, 0.5f, "%.2f");
    changed |= ImGui::Checkbox("Visualize cascades", &settings.visualizeShadowCascades);
    ImGui::EndDisabled();
    return changed;
}

bool drawLighting(RenderSettings& settings, const RenderCapabilities& capabilities) {
    bool changed =
        enumCombo("Lighting model", settings.lighting, std::array<EnumOption, 2>{{{"Fast"}, {"Physically based"}}});
    changed |= enumCombo("Environment", settings.environment,
                         std::array<EnumOption, 3>{{{"Hemisphere"},
                                                    {"Image-based (low)", capabilities.imageBasedLighting},
                                                    {"Image-based (high)", capabilities.imageBasedLighting}}});
    ImGui::BeginDisabled(settings.environment == EnvironmentQuality::Hemisphere);
    changed |= ImGui::Checkbox("Environment background", &settings.environmentBackground);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!capabilities.reflectionProbes);
    changed |= ImGui::Checkbox("Reflection probes", &settings.reflectionProbes);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!capabilities.reflectionProbes || !settings.reflectionProbes);
    changed |=
        enumCombo("Probe blending", settings.reflectionProbeBlending,
                  std::array<EnumOption, 2>{{{"At camera"}, {"Per pixel", capabilities.perPixelReflectionProbes}}});
    ImGui::EndDisabled();
    const bool localLightsAvailable = capabilities.maximumLocalLights > 0;
    changed |= enumCombo("Local lights", settings.localLights,
                         std::array<EnumOption, 2>{{{"Disabled"}, {"Clustered", localLightsAvailable}}});
    ImGui::BeginDisabled(settings.localLights == LocalLightingQuality::Disabled || !localLightsAvailable);
    int maximumLights = static_cast<int>(settings.maximumLocalLights);
    const int lightCapacity = static_cast<int>(std::max(capabilities.maximumLocalLights, 1u));
    if (ImGui::SliderInt("Maximum local lights", &maximumLights, 1, lightCapacity)) {
        settings.maximumLocalLights = static_cast<uint32_t>(maximumLights);
        changed = true;
    }
    changed |= enumCombo("Local light shadows", settings.localShadows,
                         std::array<EnumOption, 4>{{{"Off"}, {"Low"}, {"Medium"}, {"High"}}});
    ImGui::EndDisabled();
    changed |= enumCombo("Volumetric fog", settings.volumetricFog,
                         std::array<EnumOption, 4>{{{"Off"},
                                                    {"Low", capabilities.volumetricFog},
                                                    {"Medium", capabilities.volumetricFog},
                                                    {"High", capabilities.volumetricFog}}});
    if (settings.volumetricFog != VolumetricFogQuality::Off) {
        ImGui::TextDisabled("Add a Volumetric Fog component to the scene to author the medium.");
    }
    return changed;
}

bool drawScreenSpaceEffects(RenderSettings& settings, const RenderCapabilities& capabilities) {
    bool changed =
        enumCombo("Ambient occlusion", settings.ambientOcclusion,
                  std::array<EnumOption, 3>{{{"Off"},
                                             {"Half resolution", capabilities.screenSpaceAmbientOcclusion},
                                             {"Full resolution", capabilities.screenSpaceAmbientOcclusion}}});
    changed |= enumCombo("Reflections", settings.reflections,
                         std::array<EnumOption, 3>{{{"Probes only"},
                                                    {"Screen-space (half)", capabilities.screenSpaceReflections},
                                                    {"Screen-space (full)", capabilities.screenSpaceReflections}}});
    return changed;
}

bool drawBloomAndToneMapping(RenderSettings& settings) {
    bool changed = enumCombo("Bloom", settings.bloom, std::array<EnumOption, 2>{{{"Off"}, {"Downsampled chain"}}});
    ImGui::BeginDisabled(settings.bloom == BloomQuality::Off);
    changed |= ImGui::SliderFloat("Bloom intensity", &settings.bloomIntensity, 0.0f, 1.0f, "%.3f");
    changed |=
        ImGui::SliderFloat("Bloom radius", &settings.bloomFilterRadius, minimumBloomRadius, maximumBloomRadius, "%.2f");
    ImGui::EndDisabled();
    changed |=
        enumCombo("Tone mapping", settings.tonemap, std::array<EnumOption, 3>{{{"ACES"}, {"AgX"}, {"AgX (punchy)"}}});
    return changed;
}

bool drawExposure(ExposureSettings& exposure) {
    bool changed = enumCombo("Exposure mode", exposure.mode, std::array<EnumOption, 2>{{{"Automatic"}, {"Manual"}}});
    changed |= ImGui::SliderFloat("Compensation", &exposure.compensationEv, -maximumCompensationEv,
                                  maximumCompensationEv, "%.2f EV");
    ImGui::BeginDisabled(exposure.mode != ExposureMode::Manual);
    changed |= ImGui::SliderFloat("Manual exposure", &exposure.manualEv, -maximumManualEv, maximumManualEv, "%.2f EV");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(exposure.mode != ExposureMode::Auto);
    changed |= ImGui::SliderFloat("Minimum luminance", &exposure.minimumLogLuminance, -logLuminanceLimit,
                                  exposure.maximumLogLuminance - minimumLogLuminanceSpan, "%.1f EV");
    changed |= ImGui::SliderFloat("Maximum luminance", &exposure.maximumLogLuminance,
                                  exposure.minimumLogLuminance + minimumLogLuminanceSpan, logLuminanceLimit, "%.1f EV");
    changed |=
        ImGui::DragFloat("Adapt up speed", &exposure.adaptationSpeedUp, 0.01f, minimumAdaptationSpeed, 20.0f, "%.2f");
    changed |= ImGui::DragFloat("Adapt down speed", &exposure.adaptationSpeedDown, 0.01f, minimumAdaptationSpeed, 20.0f,
                                "%.2f");
    changed |= ImGui::SliderFloat("Low percentile", &exposure.lowPercentile, 0.0f,
                                  exposure.highPercentile - minimumPercentileGap, "%.2f");
    changed |= ImGui::SliderFloat("High percentile", &exposure.highPercentile,
                                  exposure.lowPercentile + minimumPercentileGap, 1.0f, "%.2f");
    ImGui::EndDisabled();
    return changed;
}

bool drawGeometryAndSubmission(RenderSettings& settings, const RenderCapabilities& capabilities) {
    bool changed = ImGui::SliderFloat("LOD error threshold", &settings.lodErrorThresholdPixels, 0.0f,
                                      maximumLodErrorPixels, "%.2f px");
    changed |= ImGui::Checkbox("Depth prepass", &settings.depthPrepass);
    ImGui::BeginDisabled(!capabilities.gpuDrivenPath);
    changed |= ImGui::Checkbox("Prefer GPU-driven submission", &settings.preferGpuDrivenSubmission);
    ImGui::EndDisabled();
    return changed;
}

bool drawBudgets(RenderPassBudgets& budgets) {
    ImGui::TextDisabled("CPU / GPU milliseconds; zero disables a check.");
    bool changed = drawBudget("Shadow", budgets.shadow);
    changed |= drawBudget("Opaque", budgets.opaque);
    changed |= drawBudget("Blended", budgets.blended);
    changed |= drawBudget("Bloom", budgets.bloom);
    changed |= drawBudget("UI", budgets.ui);
    changed |= drawBudget("Upload", budgets.upload);
    changed |= drawBudget("Temporal resolve", budgets.temporalResolve);
    changed |= drawBudget("Ambient occlusion", budgets.ambientOcclusion);
    changed |= drawBudget("Reflections", budgets.reflections);
    changed |= drawBudget("Exposure", budgets.exposure);
    changed |= drawBudget("Depth prepass", budgets.depthPrepass);
    changed |= drawBudget("Depth pyramid", budgets.depthPyramid);
    changed |= drawBudget("Light clusters", budgets.lightClusters);
    changed |= drawBudget("Environment", budgets.environment);
    changed |= drawBudget("Shadow culling", budgets.shadowCulling);
    changed |= drawBudget("Local shadows", budgets.localShadows);
    changed |= drawBudget("Probe clusters", budgets.reflectionProbeClusters);
    changed |= drawBudget("Volumetric fog", budgets.volumetricFog);
    return changed;
}

void clampBudget(RenderPassBudget& budget) {
    budget.cpuMilliseconds = std::max(budget.cpuMilliseconds, 0.0);
    budget.gpuMilliseconds = std::max(budget.gpuMilliseconds, 0.0);
}
} // namespace

namespace GraphicsSettingsEditor {
std::vector<GpuPassTiming> gpuPassTimings(const RenderStats& stats) {
    if (!stats.gpuTimingsValid) {
        return {};
    }
    // Shadow time runs from the frame start through the early GPU cull, so it
    // also covers the environment precompute, local shadows and GPU
    // shadow-cascade culling; opaque time spans the clusters, prepass, pyramid,
    // volumetric fog and ambient occlusion; blended time spans the skybox.
    return {{"Shadows", stats.gpuShadowMilliseconds},
            {"Local shadows", stats.gpuLocalShadowMilliseconds, true},
            {"Environment precompute", stats.gpuEnvironmentPrecomputeMilliseconds, true},
            {"Shadow culling", stats.gpuShadowCullingMilliseconds, true},
            {"GPU culling", stats.gpuCullingMilliseconds, true},
            {"Opaque", stats.gpuOpaqueMilliseconds},
            {"Light clusters", stats.gpuLightClusterMilliseconds, true},
            {"Probe clusters", stats.gpuReflectionProbeClusterMilliseconds, true},
            {"Volumetric fog", stats.gpuVolumetricFogMilliseconds, true},
            {"Depth prepass", stats.gpuDepthPrepassMilliseconds, true},
            {"Depth pyramid", stats.gpuDepthPyramidMilliseconds, true},
            {"Ambient occlusion", stats.gpuAmbientOcclusionMilliseconds, true},
            {"Blended", stats.gpuBlendedMilliseconds},
            {"Skybox", stats.gpuSkyboxMilliseconds, true},
            {"Reflections", stats.gpuReflectionsMilliseconds},
            {"Temporal resolve", stats.gpuTemporalMilliseconds},
            {"Exposure", stats.gpuExposureMilliseconds},
            {"Bloom", stats.gpuBloomMilliseconds},
            {"Post-process", stats.gpuPostMilliseconds},
            {"UI", stats.gpuUiMilliseconds},
            {"Frame", stats.gpuFrameMilliseconds}};
}

void clampToDocumentRanges(RenderSettings& settings) {
    settings.lodErrorThresholdPixels = std::clamp(settings.lodErrorThresholdPixels, 0.0f, maximumLodErrorPixels);
    settings.cascadeSplitLambda = std::clamp(settings.cascadeSplitLambda, 0.0f, 1.0f);
    settings.cascadeBlendFraction = std::clamp(settings.cascadeBlendFraction, 0.0f, 0.5f);
    settings.bloomIntensity = std::clamp(settings.bloomIntensity, 0.0f, 1.0f);
    settings.bloomFilterRadius = std::clamp(settings.bloomFilterRadius, minimumBloomRadius, maximumBloomRadius);
    settings.maximumLocalLights = std::max(settings.maximumLocalLights, 1u);

    DynamicResolutionSettings& dynamic = settings.dynamicResolution;
    dynamic.minimumScale = std::clamp(dynamic.minimumScale, minimumDynamicScale, 1.0f);
    dynamic.maximumScale = std::clamp(dynamic.maximumScale, dynamic.minimumScale, 1.0f);
    dynamic.targetGpuMilliseconds = std::max(dynamic.targetGpuMilliseconds, 0.1f);
    dynamic.adjustmentRate = std::clamp(dynamic.adjustmentRate, 0.001f, maximumAdjustmentRate);
    dynamic.quantization = std::clamp(dynamic.quantization, 1u, maximumQuantization);

    ExposureSettings& exposure = settings.exposure;
    exposure.compensationEv = std::clamp(exposure.compensationEv, -maximumCompensationEv, maximumCompensationEv);
    exposure.manualEv = std::clamp(exposure.manualEv, -maximumManualEv, maximumManualEv);
    exposure.minimumLogLuminance =
        std::clamp(exposure.minimumLogLuminance, -logLuminanceLimit, logLuminanceLimit - minimumLogLuminanceSpan);
    exposure.maximumLogLuminance = std::clamp(
        exposure.maximumLogLuminance, exposure.minimumLogLuminance + minimumLogLuminanceSpan, logLuminanceLimit);
    exposure.adaptationSpeedUp = std::max(exposure.adaptationSpeedUp, minimumAdaptationSpeed);
    exposure.adaptationSpeedDown = std::max(exposure.adaptationSpeedDown, minimumAdaptationSpeed);
    exposure.lowPercentile = std::clamp(exposure.lowPercentile, 0.0f, 1.0f - minimumPercentileGap);
    exposure.highPercentile = std::clamp(exposure.highPercentile, exposure.lowPercentile + minimumPercentileGap, 1.0f);

    RenderPassBudgets& budgets = settings.budgets;
    for (RenderPassBudget* budget :
         {&budgets.shadow, &budgets.opaque, &budgets.blended, &budgets.bloom, &budgets.ui, &budgets.upload,
          &budgets.temporalResolve, &budgets.ambientOcclusion, &budgets.reflections, &budgets.exposure,
          &budgets.depthPrepass, &budgets.depthPyramid, &budgets.lightClusters, &budgets.environment,
          &budgets.localShadows, &budgets.shadowCulling, &budgets.reflectionProbeClusters,
          &budgets.volumetricFog}) {
        clampBudget(*budget);
    }
}

bool drawSettings(RenderSettings& settings, const RenderCapabilities& capabilities) {
    bool changed = false;
    // Narrow docked panels still leave room for the labels.
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::CollapsingHeader("Anti-aliasing and resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= drawAntialiasingAndResolution(settings, capabilities);
    }
    if (ImGui::CollapsingHeader("Lighting and environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= drawLighting(settings, capabilities);
    }
    if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= drawShadows(settings);
    }
    if (ImGui::CollapsingHeader("Screen-space effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= drawScreenSpaceEffects(settings, capabilities);
    }
    if (ImGui::CollapsingHeader("Bloom and tone mapping")) {
        changed |= drawBloomAndToneMapping(settings);
    }
    if (ImGui::CollapsingHeader("Exposure")) {
        changed |= drawExposure(settings.exposure);
    }
    if (ImGui::CollapsingHeader("Geometry and submission")) {
        changed |= drawGeometryAndSubmission(settings, capabilities);
    }
    if (ImGui::CollapsingHeader("Pass budgets")) {
        changed |= drawBudgets(settings.budgets);
    }
    ImGui::PopItemWidth();
    if (changed) {
        clampToDocumentRanges(settings);
    }
    return changed;
}

void drawPassTimings(const RenderStats& stats) {
    const std::vector<GpuPassTiming> timings = gpuPassTimings(stats);
    if (timings.empty()) {
        ImGui::TextDisabled("GPU timings are not available yet.");
        return;
    }
    const double frameMilliseconds = std::max(stats.gpuFrameMilliseconds, 0.001);
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    // A bounded, scrolling table keeps the settings below it in view.
    constexpr float visibleRows = 9.5f;
    const ImVec2 tableSize(0.0f, ImGui::GetFrameHeight() * visibleRows);
    if (!ImGui::BeginTable("GPU pass timings", 3, flags, tableSize)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (const GpuPassTiming& timing : timings) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (timing.nested) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", timing.name);
            ImGui::Unindent();
        } else {
            ImGui::TextUnformatted(timing.name);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", timing.milliseconds);
        ImGui::TableNextColumn();
        const float share = static_cast<float>(std::clamp(timing.milliseconds / frameMilliseconds, 0.0, 1.0));
        ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), "");
    }
    ImGui::EndTable();
}
} // namespace GraphicsSettingsEditor
