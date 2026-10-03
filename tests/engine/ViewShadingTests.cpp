#include "engine/graphics/MeshPassFramePlan.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/ViewShading.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

RenderCapabilities wireframeCapabilities() {
    return {.wireframe = true};
}

// Every feature a non-Rendered mode turns off, requested on.
RenderSettings everythingRequested() {
    RenderSettings settings;
    settings.antialiasing = AntialiasingMode::Taa;
    settings.upscaler.mode = UpscalerMode::Taau;
    settings.shadows = ShadowQuality::CascadedHigh;
    settings.visualizeShadowCascades = true;
    settings.bloom = BloomQuality::Downsampled;
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.environmentBackground = true;
    settings.localLights = LocalLightingQuality::Clustered;
    settings.localShadows = LocalShadowQuality::High;
    settings.reflectionProbes = true;
    settings.indirectDiffuse = IndirectDiffuseMode::ProbeVolume;
    settings.ambientOcclusion = AmbientOcclusionQuality::FullResolution;
    settings.reflections = ReflectionQuality::ScreenSpaceFullResolution;
    settings.volumetricFog = VolumetricFogQuality::High;
    settings.motionBlur = MotionBlurQuality::High;
    settings.depthOfField.quality = DepthOfFieldQuality::High;
    settings.colorLut.enabled = true;
    settings.vignette.enabled = true;
    settings.filmGrain.enabled = true;
    settings.exposure = {.mode = ExposureMode::Auto, .compensationEv = 1.5f, .manualEv = 3.0f};
    settings.decals = DecalQuality::On;
    settings.particles = ParticleQuality::High;
    settings.lodErrorThresholdPixels = 2.0f;
    settings.depthPrepass = true;
    settings.preferGpuDrivenSubmission = true;
    return settings;
}

std::string encode(const RenderSettings& settings) {
    std::string encoded;
    expect(static_cast<bool>(serializeRenderSettings(settings, encoded)), "render settings serialize");
    return encoded;
}

void testSerializationRoundTripsEveryMode() {
    for (const ViewShading shading :
         {ViewShading::Rendered, ViewShading::MaterialPreview, ViewShading::Solid, ViewShading::Wireframe}) {
        RenderSettings settings;
        settings.viewShading = shading;
        RenderSettings decoded;
        decoded.viewShading = ViewShading::Solid == shading ? ViewShading::Wireframe : ViewShading::Solid;
        expect(static_cast<bool>(deserializeRenderSettings(encode(settings), decoded)) &&
                   decoded.viewShading == shading,
               "the view shading mode round trips");
    }
}

void testDocumentsWithoutViewShadingDrawRendered() {
    RenderSettings decoded;
    decoded.viewShading = ViewShading::Wireframe;
    expect(static_cast<bool>(deserializeRenderSettings(R"({"version": 1, "antialiasing": 1, "shadows": 1,
        "lighting": 1, "bloom": 0, "environment": 0, "localLights": 1})",
                                                       decoded)) &&
               decoded.viewShading == ViewShading::Rendered,
           "documents written before view shading load as Rendered");

    RenderSettings unknown;
    unknown.viewShading = ViewShading::Solid;
    std::string encoded = encode(unknown);
    const std::string key = "\"viewShading\": 2";
    const size_t position = encoded.find(key);
    expect(position != std::string::npos, "the document stores the mode by value");
    if (position != std::string::npos) {
        encoded.replace(position, key.size(), "\"viewShading\": 9");
    }
    RenderSettings rejected;
    const RenderConfigurationResult result = deserializeRenderSettings(encoded, rejected);
    expect(!result && result.error.find("view shading") != std::string::npos,
           "an unknown view shading mode is a descriptive load error");
}

// Other unsupported defaults (probes, probe volumes) report their own fallbacks.
bool reportsWireframeFallback(const ValidatedRenderSettings& validated) {
    return std::ranges::any_of(validated.fallbacks, [](const std::string& fallback) {
        return fallback.find("fillModeNonSolid") != std::string::npos;
    });
}

void testWireframeFallsBackWithoutFillModeNonSolid() {
    RenderSettings settings;
    settings.viewShading = ViewShading::Wireframe;
    const ValidatedRenderSettings unsupported = validateRenderSettings(settings, RenderCapabilities{});
    expect(unsupported.settings.viewShading == ViewShading::Solid, "Wireframe falls back to Solid");
    expect(reportsWireframeFallback(unsupported), "the Wireframe fallback is reported");

    const ValidatedRenderSettings supported = validateRenderSettings(settings, wireframeCapabilities());
    expect(supported.settings.viewShading == ViewShading::Wireframe && !reportsWireframeFallback(supported),
           "Wireframe is kept when the device draws lines");

    expect(resolveViewShading(ViewShading::Wireframe, {}) == ViewShading::Solid &&
               resolveViewShading(ViewShading::Solid, {}) == ViewShading::Solid &&
               resolveViewShading(ViewShading::MaterialPreview, {}) == ViewShading::MaterialPreview &&
               resolveViewShading(ViewShading::Rendered, {}) == ViewShading::Rendered,
           "only Wireframe depends on the wireframe capability");
    // Settings forced past validation (ZENITH_FORCE_VIEW_SHADING) still fall back.
    const RenderSettings frame = viewShadingFrameSettings(settings, RenderCapabilities{});
    expect(frame.viewShading == ViewShading::Solid && !drawsWireframeOverlay(frame.viewShading),
           "unvalidated Wireframe records as Solid without the capability");
}

void testRenderedFramesAreUnchanged() {
    const RenderSettings requested = everythingRequested();
    expect(encode(viewShadingFrameSettings(requested, wireframeCapabilities())) == encode(requested),
           "Rendered records with exactly the requested settings");
}

void expectSceneLightingAndCameraEffectsOff(const RenderSettings& frame, const char* message) {
    const bool sceneLightingOff = frame.shadows == ShadowQuality::Off && !frame.visualizeShadowCascades &&
                                  frame.localLights == LocalLightingQuality::Disabled &&
                                  frame.localShadows == LocalShadowQuality::Off &&
                                  frame.environment == EnvironmentQuality::Hemisphere && !frame.environmentBackground &&
                                  !frame.reflectionProbes && frame.indirectDiffuse == IndirectDiffuseMode::Environment;
    const bool screenSpaceOff = frame.ambientOcclusion == AmbientOcclusionQuality::Off &&
                                frame.reflections == ReflectionQuality::ProbesOnly &&
                                frame.volumetricFog == VolumetricFogQuality::Off;
    const bool cameraEffectsOff = frame.bloom == BloomQuality::Off && frame.motionBlur == MotionBlurQuality::Off &&
                                  frame.depthOfField.quality == DepthOfFieldQuality::Off && !frame.colorLut.enabled &&
                                  !frame.vignette.enabled && !frame.filmGrain.enabled &&
                                  frame.exposure.mode == ExposureMode::Manual && frame.exposure.manualEv == 0.0f &&
                                  frame.exposure.compensationEv == 0.0f;
    const bool geometryKept =
        frame.depthPrepass && frame.preferGpuDrivenSubmission && frame.lodErrorThresholdPixels == 2.0f;
    expect(sceneLightingOff && screenSpaceOff && cameraEffectsOff && geometryKept, message);
}

void testMaterialPreviewFeatureSet() {
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::MaterialPreview;
    const RenderSettings frame = viewShadingFrameSettings(requested, wireframeCapabilities());
    expectSceneLightingAndCameraEffectsOff(frame, "Material Preview drops scene lighting and camera effects");
    expect(frame.decals == DecalQuality::On && frame.particles == ParticleQuality::High,
           "Material Preview keeps decals and particles");
    expect(frame.antialiasing == AntialiasingMode::Taa && frame.upscaler.mode == UpscalerMode::Taau,
           "Material Preview keeps temporal anti-aliasing");
    expect(!usesSolidSurfaceShading(frame.viewShading) && !drawsWireframeOverlay(frame.viewShading) &&
               usesStudioLighting(frame.viewShading),
           "Material Preview shades materials under the studio rig");
}

void testSolidAndWireframeFeatureSets() {
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::Solid;
    const RenderSettings solid = viewShadingFrameSettings(requested, wireframeCapabilities());
    expectSceneLightingAndCameraEffectsOff(solid, "Solid drops scene lighting and camera effects");
    expect(solid.decals == DecalQuality::Off && solid.particles == ParticleQuality::Off,
           "Solid draws no decals or particles");
    expect(solid.antialiasing == AntialiasingMode::Taa, "Solid keeps TAA");
    expect(usesSolidSurfaceShading(solid.viewShading) && !drawsWireframeOverlay(solid.viewShading),
           "Solid draws clay surfaces without edges");

    requested.viewShading = ViewShading::Wireframe;
    const RenderSettings wireframe = viewShadingFrameSettings(requested, wireframeCapabilities());
    expectSceneLightingAndCameraEffectsOff(wireframe, "Wireframe drops scene lighting and camera effects");
    expect(wireframe.antialiasing == AntialiasingMode::Fxaa && wireframe.upscaler.mode == UpscalerMode::Off,
           "Wireframe trades TAA and TAA upsampling for FXAA");
    expect(usesSolidSurfaceShading(wireframe.viewShading) && drawsWireframeOverlay(wireframe.viewShading),
           "Wireframe draws edges over clay surfaces");

    requested.upscaler.mode = UpscalerMode::Fsr1;
    expect(viewShadingFrameSettings(requested, wireframeCapabilities()).upscaler.mode == UpscalerMode::Fsr1,
           "Wireframe keeps a spatial upscaler");
}

void testFramePlanSkipsScreenSpacePasses() {
    RenderSettings requested = everythingRequested();
    const RenderCapabilities capabilities{.descriptorIndexing = true,
                                          .multiDrawIndirect = true,
                                          .indirectDrawCount = true,
                                          .gpuDrivenPath = true,
                                          .wireframe = true};
    const MeshPassFramePlan rendered =
        planMeshPassFrame(viewShadingFrameSettings(requested, capabilities), capabilities, {});
    expect(rendered.ambientOcclusion != AmbientOcclusionQuality::Off && rendered.screenSpaceReflections &&
               rendered.perPixelReflectionProbes,
           "Rendered plans the requested screen-space passes");
    requested.viewShading = ViewShading::Solid;
    const MeshPassFramePlan solid =
        planMeshPassFrame(viewShadingFrameSettings(requested, capabilities), capabilities, {});
    expect(solid.depthPrepass && solid.gpuDrivenSubmission, "Solid keeps the prepass and GPU-driven submission");
    expect(solid.ambientOcclusion == AmbientOcclusionQuality::Off && !solid.screenSpaceReflections &&
               !solid.perPixelReflectionProbes,
           "Solid plans no screen-space or probe passes");
}

bool nearlyUnit(const glm::vec3& value) {
    return std::abs(glm::length(value) - 1.0f) < 1e-4f;
}

void testStudioRigFollowsTheCamera() {
    const glm::mat4 lookingDownZ = glm::inverse(
        glm::lookAtRH(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const StudioLighting front = studioLighting(lookingDownZ);
    expect(nearlyUnit(front.keyDirection), "the key direction is normalized");
    expect(front.keyDirection.y > 0.0f && front.keyDirection.z > 0.0f,
           "the key light sits above and behind the camera (toward the viewer)");
    expect(front.keyRadiance.x > 0.0f && front.skyColor.y > front.groundColor.y,
           "the rig has a key light and a sky brighter than the ground");
    expect(front.specularLevels.front().x > front.specularLevels.back().x,
           "smooth surfaces reflect the brighter highlight");

    const glm::mat4 lookingDownX = glm::inverse(
        glm::lookAtRH(glm::vec3(5.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const StudioLighting side = studioLighting(lookingDownX);
    expect(side.keyDirection.x > 0.0f && std::abs(side.keyDirection.y - front.keyDirection.y) < 1e-4f,
           "the key light turns with the camera");
}

void testScopedSettingsRestoreTheRequest() {
    RenderRuntime runtime;
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::Rendered;
    runtime.setSettings(requested);
    {
        const ScopedViewShadingSettings scope{runtime};
        expect(encode(runtime.settings()) == encode(requested), "Rendered frames keep the runtime settings");
    }
    requested.viewShading = ViewShading::Wireframe;
    runtime.setSettings(requested);
    {
        const ScopedViewShadingSettings scope{runtime};
        // A default runtime reports no wireframe capability.
        expect(runtime.settings().viewShading == ViewShading::Solid && runtime.settings().shadows == ShadowQuality::Off,
               "the frame records with the resolved mode's settings");
    }
    expect(encode(runtime.settings()) == encode(requested), "the requested settings return after the frame");
}
} // namespace

int main() {
    testSerializationRoundTripsEveryMode();
    testDocumentsWithoutViewShadingDrawRendered();
    testWireframeFallsBackWithoutFillModeNonSolid();
    testRenderedFramesAreUnchanged();
    testMaterialPreviewFeatureSet();
    testSolidAndWireframeFeatureSets();
    testFramePlanSkipsScreenSpacePasses();
    testStudioRigFollowsTheCamera();
    testScopedSettingsRestoreTheRequest();
    if (failures != 0) {
        std::cerr << failures << " view shading test(s) failed\n";
        return 1;
    }
    std::cout << "View shading tests passed\n";
    return 0;
}
