#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/settings/EditorRenderSettings.hpp"

#include "support/Doctest.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("EditorRenderSettings");

namespace {

std::string serialized(const RenderSettings& settings) {
    std::string output;
    CHECK_MESSAGE(serializeRenderSettings(settings, output).success, "render settings should serialize");
    return output;
}

RenderSettings editedSettings() {
    RenderSettings settings;
    settings.antialiasing = AntialiasingMode::Taa;
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.bloom = BloomQuality::Downsampled;
    settings.bloomIntensity = 0.12f;
    settings.exposure.mode = ExposureMode::Manual;
    settings.exposure.manualEv = 2.5f;
    settings.maximumLocalLights = 256;
    settings.depthPrepass = false;
    settings.budgets.depthPrepass.gpuMilliseconds = 0.5;
    return settings;
}

} // namespace

TEST_CASE("EditorRenderSettings: standalone document") {
    const testsupport::TempDirectory directory("render-settings-standalone");
    const std::filesystem::path path = directory / "render-settings.json";
    EditorRenderSettings settings(ProjectConfig{}, path);
    CHECK_MESSAGE((!settings.savesToProject() && settings.storagePath() == path),
                  "no project file should store standalone");
    CHECK_MESSAGE(settings.load().success, "a missing standalone document should keep defaults");
    CHECK_MESSAGE(serialized(settings.requested()) == serialized(RenderSettings{}),
                  "defaults should be requested initially");

    settings.request(editedSettings());
    CHECK_MESSAGE(settings.unsaved(), "a new request should be unsaved");
    CHECK_MESSAGE((settings.load().success && !settings.unsaved() &&
                   serialized(settings.requested()) == serialized(RenderSettings{})),
                  "reverting before the first save should restore the initial settings");

    settings.request(editedSettings());
    CHECK_MESSAGE((settings.save().success && !settings.unsaved()), "standalone settings should save");

    EditorRenderSettings restored(ProjectConfig{}, path);
    CHECK_MESSAGE(restored.load().success, "saved standalone settings should load");
    CHECK_MESSAGE(serialized(restored.requested()) == serialized(editedSettings()),
                  "standalone settings should round trip");

    testsupport::writeText(path, "{\"version\": 1, \"antialiasing\": 9}");
    CHECK_MESSAGE(!restored.load().success, "an invalid document should fail descriptively");
    CHECK_MESSAGE(serialized(restored.requested()) == serialized(editedSettings()),
                  "a failed load should keep the current request");
}

TEST_CASE("EditorRenderSettings: project file") {
    const testsupport::TempDirectory directory("render-settings-project");
    ProjectConfig project{
        .displayName = "Render settings test", .assetRoot = directory / "assets", .sceneRoot = directory / "scenes"};
    const std::filesystem::path projectPath = directory / "project.zenith-project";
    CHECK_MESSAGE(saveProjectConfig(projectPath, project).success, "test project should save");
    ProjectConfig loaded;
    CHECK_MESSAGE(loadProjectConfig(projectPath, loaded).success, "test project should load");

    EditorRenderSettings settings(loaded, directory / "unused.json");
    CHECK_MESSAGE(settings.savesToProject(), "a loaded project should own its render settings");
    CHECK_MESSAGE(settings.load().success, "project render settings should load");
    settings.request(editedSettings());
    CHECK_MESSAGE(settings.save().success, "project render settings should save");
    CHECK_MESSAGE(!std::filesystem::exists(directory / "unused.json"),
                  "project saves should not write the standalone file");

    ProjectConfig reloaded;
    CHECK_MESSAGE(loadProjectConfig(projectPath, reloaded).success, "saved project should load");
    CHECK_MESSAGE(serialized(reloaded.renderSettings) == serialized(editedSettings()),
                  "render settings should persist in the project file");
    CHECK_MESSAGE(reloaded.displayName == project.displayName,
                  "saving render settings should keep other project fields");

    // A session created from a stale copy still sees the saved file.
    EditorRenderSettings stale(loaded, directory / "unused.json");
    CHECK_MESSAGE(stale.load().success, "stale project copy should reload the file");
    CHECK_MESSAGE(serialized(stale.requested()) == serialized(editedSettings()), "load should prefer the file on disk");
}

TEST_CASE("EditorRenderSettings: capability fallbacks") {
    RenderRuntime rendering;
    EditorRenderSettings settings(ProjectConfig{}, {});
    settings.request(editedSettings());
    // Default capabilities have no image-based lighting.
    const std::vector<std::string> fallbacks = settings.apply(rendering);
    CHECK_MESSAGE(!fallbacks.empty(), "unsupported features should report fallbacks");
    CHECK_MESSAGE(rendering.settings().environment == EnvironmentQuality::Hemisphere,
                  "the renderer should receive the validated settings");
    CHECK_MESSAGE(settings.requested().environment == EnvironmentQuality::ImageBasedHigh,
                  "the request should keep what the user chose");
}

TEST_CASE("EditorRenderSettings: document range clamping") {
    RenderSettings settings;
    settings.lodErrorThresholdPixels = 1000.0f;
    settings.cascadeBlendFraction = 4.0f;
    settings.bloomFilterRadius = 0.0f;
    settings.maximumLocalLights = 0;
    settings.dynamicResolution.minimumScale = 0.9f;
    settings.dynamicResolution.maximumScale = 0.5f;
    settings.dynamicResolution.quantization = 0;
    settings.exposure.minimumLogLuminance = 4.0f;
    settings.exposure.maximumLogLuminance = 4.5f;
    settings.exposure.lowPercentile = 0.9f;
    settings.exposure.highPercentile = 0.5f;
    settings.exposure.adaptationSpeedUp = 0.0f;
    settings.budgets.environment.gpuMilliseconds = -1.0;
    settings.depthOfField.focusDistance = 0.0f;
    settings.depthOfField.fStop = 1000.0f;
    settings.depthOfField.sensorHeightMillimeters = 0.0f;
    settings.depthOfField.maximumCocRadiusPixels = 500.0f;
    GraphicsSettingsEditor::clampToDocumentRanges(settings);
    RenderSettings restored;
    CHECK_MESSAGE(deserializeRenderSettings(serialized(settings), restored).success,
                  "clamped settings should be accepted by the render-settings document");
    CHECK_MESSAGE(serialized(restored) == serialized(settings), "clamped settings should round trip unchanged");
}

TEST_CASE("EditorRenderSettings: pass timings") {
    RenderStats stats;
    CHECK_MESSAGE(GraphicsSettingsEditor::gpuPassTimings(stats).empty(), "invalid timings should produce no rows");
    stats.gpuTimingsValid = true;
    stats.gpuFrameMilliseconds = 2.0;
    stats.gpuDepthPrepassMilliseconds = 0.25;
    stats.gpuSkyboxMilliseconds = 0.05;
    stats.gpuShadowCullingMilliseconds = 0.02;
    const std::vector<GpuPassTiming> timings = GraphicsSettingsEditor::gpuPassTimings(stats);
    CHECK_MESSAGE(
        (!timings.empty() && std::strcmp(timings.back().name, "Frame") == 0 && timings.back().milliseconds == 2.0),
        "the table should end with the whole frame");
    bool foundPrepass = false;
    bool foundShadowCulling = false;
    for (const GpuPassTiming& timing : timings) {
        if (std::strcmp(timing.name, "Depth prepass") == 0) {
            foundPrepass = timing.milliseconds == 0.25 && timing.nested;
        }
        if (std::strcmp(timing.name, "Shadow culling") == 0) {
            foundShadowCulling = timing.milliseconds == 0.02 && timing.nested;
        }
    }
    CHECK_MESSAGE(foundPrepass, "the prepass should be listed within opaque time");
    CHECK_MESSAGE(foundShadowCulling, "shadow-cascade culling should be listed within shadow time");
}

TEST_SUITE_END();
