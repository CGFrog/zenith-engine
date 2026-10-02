#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/settings/EditorRenderSettings.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string serialized(const RenderSettings& settings) {
    std::string output;
    expect(serializeRenderSettings(settings, output).success, "render settings should serialize");
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

void testStandaloneDocument(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / "render-settings.json";
    EditorRenderSettings settings(ProjectConfig{}, path);
    expect(!settings.savesToProject() && settings.storagePath() == path, "no project file should store standalone");
    expect(settings.load().success, "a missing standalone document should keep defaults");
    expect(serialized(settings.requested()) == serialized(RenderSettings{}), "defaults should be requested initially");

    settings.request(editedSettings());
    expect(settings.unsaved(), "a new request should be unsaved");
    expect(settings.load().success && !settings.unsaved() &&
               serialized(settings.requested()) == serialized(RenderSettings{}),
           "reverting before the first save should restore the initial settings");

    settings.request(editedSettings());
    expect(settings.save().success && !settings.unsaved(), "standalone settings should save");

    EditorRenderSettings restored(ProjectConfig{}, path);
    expect(restored.load().success, "saved standalone settings should load");
    expect(serialized(restored.requested()) == serialized(editedSettings()), "standalone settings should round trip");

    {
        std::ofstream malformed(path, std::ios::trunc);
        malformed << "{\"version\": 1, \"antialiasing\": 9}";
    }
    expect(!restored.load().success, "an invalid document should fail descriptively");
    expect(serialized(restored.requested()) == serialized(editedSettings()),
           "a failed load should keep the current request");
}

void testProjectFile(const std::filesystem::path& directory) {
    ProjectConfig project{
        .displayName = "Render settings test", .assetRoot = directory / "assets", .sceneRoot = directory / "scenes"};
    const std::filesystem::path projectPath = directory / "project.zenith-project";
    expect(saveProjectConfig(projectPath, project).success, "test project should save");
    ProjectConfig loaded;
    expect(loadProjectConfig(projectPath, loaded).success, "test project should load");

    EditorRenderSettings settings(loaded, directory / "unused.json");
    expect(settings.savesToProject(), "a loaded project should own its render settings");
    expect(settings.load().success, "project render settings should load");
    settings.request(editedSettings());
    expect(settings.save().success, "project render settings should save");
    expect(!std::filesystem::exists(directory / "unused.json"), "project saves should not write the standalone file");

    ProjectConfig reloaded;
    expect(loadProjectConfig(projectPath, reloaded).success, "saved project should load");
    expect(serialized(reloaded.renderSettings) == serialized(editedSettings()),
           "render settings should persist in the project file");
    expect(reloaded.displayName == project.displayName, "saving render settings should keep other project fields");

    // A session created from a stale copy still sees the saved file.
    EditorRenderSettings stale(loaded, directory / "unused.json");
    expect(stale.load().success, "stale project copy should reload the file");
    expect(serialized(stale.requested()) == serialized(editedSettings()), "load should prefer the file on disk");
}

void testCapabilityFallbacks() {
    RenderRuntime rendering;
    EditorRenderSettings settings(ProjectConfig{}, {});
    settings.request(editedSettings());
    // Default capabilities have no image-based lighting.
    const std::vector<std::string> fallbacks = settings.apply(rendering);
    expect(!fallbacks.empty(), "unsupported features should report fallbacks");
    expect(rendering.settings().environment == EnvironmentQuality::Hemisphere,
           "the renderer should receive the validated settings");
    expect(settings.requested().environment == EnvironmentQuality::ImageBasedHigh,
           "the request should keep what the user chose");
}

void testDocumentRangeClamping() {
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
    expect(deserializeRenderSettings(serialized(settings), restored).success,
           "clamped settings should be accepted by the render-settings document");
    expect(serialized(restored) == serialized(settings), "clamped settings should round trip unchanged");
}

void testPassTimings() {
    RenderStats stats;
    expect(GraphicsSettingsEditor::gpuPassTimings(stats).empty(), "invalid timings should produce no rows");
    stats.gpuTimingsValid = true;
    stats.gpuFrameMilliseconds = 2.0;
    stats.gpuDepthPrepassMilliseconds = 0.25;
    stats.gpuSkyboxMilliseconds = 0.05;
    stats.gpuShadowCullingMilliseconds = 0.02;
    const std::vector<GpuPassTiming> timings = GraphicsSettingsEditor::gpuPassTimings(stats);
    expect(!timings.empty() && std::strcmp(timings.back().name, "Frame") == 0 && timings.back().milliseconds == 2.0,
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
    expect(foundPrepass, "the prepass should be listed within opaque time");
    expect(foundShadowCulling, "shadow-cascade culling should be listed within shadow time");
}
} // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "zenith_editor_render_settings_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    testStandaloneDocument(directory);
    testProjectFile(directory);
    testCapabilityFallbacks();
    testDocumentRangeClamping();
    testPassTimings();

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
