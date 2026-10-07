#include "engine/graphics/MeshPassFramePlan.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/ViewShading.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

TEST_SUITE_BEGIN("ViewShading");

namespace {

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
    CHECK_MESSAGE((static_cast<bool>(serializeRenderSettings(settings, encoded))), "render settings serialize");
    return encoded;
}

TEST_CASE("ViewShading: serialization round trips every mode") {
    for (const ViewShading shading :
         {ViewShading::Rendered, ViewShading::MaterialPreview, ViewShading::Solid, ViewShading::Wireframe}) {
        RenderSettings settings;
        settings.viewShading = shading;
        RenderSettings decoded;
        decoded.viewShading = ViewShading::Solid == shading ? ViewShading::Wireframe : ViewShading::Solid;
        CHECK_MESSAGE(
            (static_cast<bool>(deserializeRenderSettings(encode(settings), decoded)) && decoded.viewShading == shading),
            "the view shading mode round trips");
    }
}

TEST_CASE("ViewShading: documents without view shading draw rendered") {
    RenderSettings decoded;
    decoded.viewShading = ViewShading::Wireframe;
    CHECK_MESSAGE((static_cast<bool>(deserializeRenderSettings(R"({"version": 1, "antialiasing": 1, "shadows": 1,
        "lighting": 1, "bloom": 0, "environment": 0, "localLights": 1})",
                                                               decoded)) &&
                   decoded.viewShading == ViewShading::Rendered),
                  "documents written before view shading load as Rendered");

    RenderSettings unknown;
    unknown.viewShading = ViewShading::Solid;
    std::string encoded = encode(unknown);
    const std::string key = "\"viewShading\": 2";
    const size_t position = encoded.find(key);
    CHECK_MESSAGE(position != std::string::npos, "the document stores the mode by value");
    if (position != std::string::npos) {
        encoded.replace(position, key.size(), "\"viewShading\": 9");
    }
    RenderSettings rejected;
    const RenderConfigurationResult result = deserializeRenderSettings(encoded, rejected);
    CHECK_MESSAGE((!result && result.error.find("view shading") != std::string::npos),
                  "an unknown view shading mode is a descriptive load error");
}

// Other unsupported defaults (probes, probe volumes) report their own fallbacks.
bool reportsWireframeFallback(const ValidatedRenderSettings& validated) {
    return std::ranges::any_of(validated.fallbacks, [](const std::string& fallback) {
        return fallback.find("fillModeNonSolid") != std::string::npos;
    });
}

TEST_CASE("ViewShading: wireframe falls back without fill mode non solid") {
    RenderSettings settings;
    settings.viewShading = ViewShading::Wireframe;
    const ValidatedRenderSettings unsupported = validateRenderSettings(settings, RenderCapabilities{});
    CHECK_MESSAGE(unsupported.settings.viewShading == ViewShading::Solid, "Wireframe falls back to Solid");
    CHECK_MESSAGE(reportsWireframeFallback(unsupported), "the Wireframe fallback is reported");

    const ValidatedRenderSettings supported = validateRenderSettings(settings, wireframeCapabilities());
    CHECK_MESSAGE((supported.settings.viewShading == ViewShading::Wireframe && !reportsWireframeFallback(supported)),
                  "Wireframe is kept when the device draws lines");

    CHECK_MESSAGE((resolveViewShading(ViewShading::Wireframe, {}) == ViewShading::Solid &&
                   resolveViewShading(ViewShading::Solid, {}) == ViewShading::Solid &&
                   resolveViewShading(ViewShading::MaterialPreview, {}) == ViewShading::MaterialPreview &&
                   resolveViewShading(ViewShading::Rendered, {}) == ViewShading::Rendered),
                  "only Wireframe depends on the wireframe capability");
    // Settings forced past validation (ZENITH_FORCE_VIEW_SHADING) still fall back.
    const RenderSettings frame = viewShadingFrameSettings(settings, RenderCapabilities{});
    CHECK_MESSAGE((frame.viewShading == ViewShading::Solid && !drawsWireframeOverlay(frame.viewShading)),
                  "unvalidated Wireframe records as Solid without the capability");
}

TEST_CASE("ViewShading: rendered frames are unchanged") {
    const RenderSettings requested = everythingRequested();
    CHECK_MESSAGE(encode(viewShadingFrameSettings(requested, wireframeCapabilities())) == encode(requested),
                  "Rendered records with exactly the requested settings");
}

void checkSceneLightingAndCameraEffectsOff(const RenderSettings& frame, const char* message) {
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
    const bool geometryKept = frame.lodErrorThresholdPixels == 2.0f;
    CHECK_MESSAGE((sceneLightingOff && screenSpaceOff && cameraEffectsOff && geometryKept), message);
}

TEST_CASE("ViewShading: material preview feature set") {
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::MaterialPreview;
    const RenderSettings frame = viewShadingFrameSettings(requested, wireframeCapabilities());
    checkSceneLightingAndCameraEffectsOff(frame, "Material Preview drops scene lighting and camera effects");
    CHECK_MESSAGE((frame.decals == DecalQuality::On && frame.particles == ParticleQuality::High),
                  "Material Preview keeps decals and particles");
    CHECK_MESSAGE((frame.antialiasing == AntialiasingMode::Taa && frame.upscaler.mode == UpscalerMode::Taau),
                  "Material Preview keeps temporal anti-aliasing");
    CHECK_MESSAGE((!usesSolidSurfaceShading(frame.viewShading) && !drawsWireframeOverlay(frame.viewShading) &&
                   usesStudioLighting(frame.viewShading)),
                  "Material Preview shades materials under the studio rig");
    CHECK_MESSAGE((frame.depthPrepass && frame.preferGpuDrivenSubmission),
                  "Material Preview keeps the prepass and GPU culling");
}

TEST_CASE("ViewShading: solid and wireframe feature sets") {
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::Solid;
    const RenderSettings solid = viewShadingFrameSettings(requested, wireframeCapabilities());
    checkSceneLightingAndCameraEffectsOff(solid, "Solid drops scene lighting and camera effects");
    CHECK_MESSAGE((solid.decals == DecalQuality::Off && solid.particles == ParticleQuality::Off),
                  "Solid draws no decals or particles");
    CHECK_MESSAGE(solid.antialiasing == AntialiasingMode::Taa, "Solid keeps TAA");
    CHECK_MESSAGE((usesSolidSurfaceShading(solid.viewShading) && !drawsWireframeOverlay(solid.viewShading)),
                  "Solid draws clay surfaces without edges");
    CHECK_MESSAGE((drawsSurfaces(solid.viewShading) && solid.depthPrepass && solid.preferGpuDrivenSubmission),
                  "Solid keeps the prepass and GPU culling");

    requested.viewShading = ViewShading::Wireframe;
    const RenderSettings wireframe = viewShadingFrameSettings(requested, wireframeCapabilities());
    checkSceneLightingAndCameraEffectsOff(wireframe, "Wireframe drops scene lighting and camera effects");
    CHECK_MESSAGE((wireframe.antialiasing == AntialiasingMode::Fxaa && wireframe.upscaler.mode == UpscalerMode::Off),
                  "Wireframe trades TAA and TAA upsampling for FXAA");
    CHECK_MESSAGE((drawsWireframeOverlay(wireframe.viewShading) && !drawsSurfaces(wireframe.viewShading)),
                  "Wireframe draws only edges, with no shaded surfaces");
    CHECK_MESSAGE((!wireframe.depthPrepass && !wireframe.preferGpuDrivenSubmission),
                  "Wireframe draws no surface depth and culls nothing by occlusion");

    requested.upscaler.mode = UpscalerMode::Fsr1;
    CHECK_MESSAGE(viewShadingFrameSettings(requested, wireframeCapabilities()).upscaler.mode == UpscalerMode::Fsr1,
                  "Wireframe keeps a spatial upscaler");
}

TEST_CASE("ViewShading: frame plan skips screen space passes") {
    RenderSettings requested = everythingRequested();
    const RenderCapabilities capabilities{.descriptorIndexing = true,
                                          .multiDrawIndirect = true,
                                          .indirectDrawCount = true,
                                          .gpuDrivenPath = true,
                                          .wireframe = true};
    const MeshPassFramePlan rendered =
        planMeshPassFrame(viewShadingFrameSettings(requested, capabilities), capabilities, {});
    CHECK_MESSAGE((rendered.ambientOcclusion != AmbientOcclusionQuality::Off && rendered.screenSpaceReflections &&
                   rendered.perPixelReflectionProbes),
                  "Rendered plans the requested screen-space passes");
    requested.viewShading = ViewShading::Solid;
    const MeshPassFramePlan solid =
        planMeshPassFrame(viewShadingFrameSettings(requested, capabilities), capabilities, {});
    CHECK_MESSAGE((solid.depthPrepass && solid.gpuDrivenSubmission),
                  "Solid keeps the prepass and GPU-driven submission");
    CHECK_MESSAGE((solid.ambientOcclusion == AmbientOcclusionQuality::Off && !solid.screenSpaceReflections &&
                   !solid.perPixelReflectionProbes),
                  "Solid plans no screen-space or probe passes");
    requested.viewShading = ViewShading::Wireframe;
    const MeshPassFramePlan wireframe =
        planMeshPassFrame(viewShadingFrameSettings(requested, capabilities), capabilities, {});
    CHECK_MESSAGE((!wireframe.depthPrepass && !wireframe.gpuDrivenSubmission),
                  "Wireframe plans no prepass and no occlusion-culled GPU-driven submission");
    CHECK_MESSAGE(!planMeshPassFrame(requested, capabilities, {.forceGpuDrivenSubmission = true}).gpuDrivenSubmission,
                  "Wireframe stays on CPU submission when GPU-driven submission is forced");
}

bool nearlyUnit(const glm::vec3& value) {
    return std::abs(glm::length(value) - 1.0f) < 1e-4f;
}

TEST_CASE("ViewShading: studio rig follows the camera") {
    const glm::mat4 lookingDownZ = glm::inverse(
        glm::lookAtRH(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const StudioLighting front = studioLighting(lookingDownZ);
    CHECK_MESSAGE(nearlyUnit(front.keyDirection), "the key direction is normalized");
    CHECK_MESSAGE((front.keyDirection.y > 0.0f && front.keyDirection.z > 0.0f),
                  "the key light sits above and behind the camera (toward the viewer)");
    CHECK_MESSAGE((front.keyRadiance.x > 0.0f && front.skyColor.y > front.groundColor.y),
                  "the rig has a key light and a sky brighter than the ground");
    CHECK_MESSAGE(front.specularLevels.front().x > front.specularLevels.back().x,
                  "smooth surfaces reflect the brighter highlight");

    const glm::mat4 lookingDownX = glm::inverse(
        glm::lookAtRH(glm::vec3(5.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const StudioLighting side = studioLighting(lookingDownX);
    CHECK_MESSAGE((side.keyDirection.x > 0.0f && std::abs(side.keyDirection.y - front.keyDirection.y) < 1e-4f),
                  "the key light turns with the camera");
}

TEST_CASE("ViewShading: scoped settings restore the request") {
    RenderRuntime runtime;
    RenderSettings requested = everythingRequested();
    requested.viewShading = ViewShading::Rendered;
    runtime.setSettings(requested);
    {
        const ScopedViewShadingSettings scope{runtime};
        CHECK_MESSAGE(encode(runtime.settings()) == encode(requested), "Rendered frames keep the runtime settings");
    }
    requested.viewShading = ViewShading::Wireframe;
    runtime.setSettings(requested);
    {
        const ScopedViewShadingSettings scope{runtime};
        // A default runtime reports no wireframe capability.
        CHECK_MESSAGE(
            (runtime.settings().viewShading == ViewShading::Solid && runtime.settings().shadows == ShadowQuality::Off),
            "the frame records with the resolved mode's settings");
    }
    CHECK_MESSAGE(encode(runtime.settings()) == encode(requested), "the requested settings return after the frame");
}
} // namespace

TEST_SUITE_END();
