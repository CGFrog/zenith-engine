#include "ViewShading.hpp"

#include <utility>

namespace {
// Calibrated so a 0.8 clay albedo under the key light and hemisphere sits a
// little above middle grey at 0 EV.
constexpr float studioKeyIntensity = 2.6f;
constexpr glm::vec3 studioSky{0.46f, 0.48f, 0.52f};
constexpr glm::vec3 studioGround{0.16f, 0.15f, 0.14f};
// Soft overhead light reflected by smooth surfaces.
constexpr glm::vec3 studioHighlight{0.9f, 0.92f, 0.95f};
constexpr float studioSpecularIntensity = 0.6f;
// Key light offsets in camera space (right, up, back), before normalizing.
constexpr float studioKeyRight = -0.35f;
constexpr float studioKeyUp = 0.6f;
constexpr float studioKeyBack = 1.0f;

void disableSceneLightingAndEffects(RenderSettings& settings) {
    settings.shadows = ShadowQuality::Off;
    settings.visualizeShadowCascades = false;
    settings.localLights = LocalLightingQuality::Disabled;
    settings.localShadows = LocalShadowQuality::Off;
    settings.environment = EnvironmentQuality::Hemisphere;
    settings.environmentBackground = false;
    settings.reflectionProbes = false;
    settings.indirectDiffuse = IndirectDiffuseMode::Environment;
    settings.ambientOcclusion = AmbientOcclusionQuality::Off;
    settings.reflections = ReflectionQuality::ProbesOnly;
    settings.volumetricFog = VolumetricFogQuality::Off;
}

void disableCameraEffects(RenderSettings& settings) {
    settings.bloom = BloomQuality::Off;
    settings.motionBlur = MotionBlurQuality::Off;
    settings.depthOfField.quality = DepthOfFieldQuality::Off;
    settings.colorLut.enabled = false;
    settings.vignette.enabled = false;
    settings.filmGrain.enabled = false;
    settings.exposure.mode = ExposureMode::Manual;
    settings.exposure.manualEv = 0.0f;
    settings.exposure.compensationEv = 0.0f;
}
} // namespace

ViewShading resolveViewShading(ViewShading requested, const RenderCapabilities& capabilities) {
    if (requested == ViewShading::Wireframe && !capabilities.wireframe) {
        return ViewShading::Solid;
    }
    return requested;
}

RenderSettings viewShadingFrameSettings(const RenderSettings& settings, const RenderCapabilities& capabilities) {
    RenderSettings frame = settings;
    frame.viewShading = resolveViewShading(settings.viewShading, capabilities);
    if (frame.viewShading == ViewShading::Rendered) {
        return frame;
    }
    disableSceneLightingAndEffects(frame);
    disableCameraEffects(frame);
    if (frame.viewShading == ViewShading::MaterialPreview) {
        return frame;
    }
    frame.decals = DecalQuality::Off;
    frame.particles = ParticleQuality::Off;
    if (frame.viewShading == ViewShading::Wireframe) {
        // Edges only, every edge shown: no surface depth is drawn and nothing
        // is occlusion culled.
        frame.depthPrepass = false;
        frame.preferGpuDrivenSubmission = false;
        if (frame.antialiasing == AntialiasingMode::Taa) {
            frame.antialiasing = AntialiasingMode::Fxaa;
            if (frame.upscaler.mode == UpscalerMode::Taau) {
                frame.upscaler.mode = UpscalerMode::Off;
            }
        }
    }
    return frame;
}

bool usesSolidSurfaceShading(ViewShading shading) {
    return shading == ViewShading::Solid || shading == ViewShading::Wireframe;
}

bool drawsSurfaces(ViewShading shading) {
    return shading != ViewShading::Wireframe;
}

bool drawsWireframeOverlay(ViewShading shading) {
    return shading == ViewShading::Wireframe;
}

bool usesStudioLighting(ViewShading shading) {
    return shading != ViewShading::Rendered;
}

StudioLighting studioLighting(const glm::mat4& cameraWorld) {
    const glm::vec3 right = glm::normalize(glm::vec3(cameraWorld[0]));
    const glm::vec3 up = glm::normalize(glm::vec3(cameraWorld[1]));
    const glm::vec3 back = glm::normalize(glm::vec3(cameraWorld[2]));
    StudioLighting lighting{.keyDirection =
                                glm::normalize(right * studioKeyRight + up * studioKeyUp + back * studioKeyBack),
                            .keyRadiance = glm::vec3(studioKeyIntensity),
                            .skyColor = studioSky,
                            .groundColor = studioGround,
                            .diffuseIntensity = 1.0f,
                            .specularIntensity = studioSpecularIntensity};
    const glm::vec3 average = (studioSky + studioGround) * 0.5f;
    for (size_t level = 0; level < lighting.specularLevels.size(); ++level) {
        const float roughness = static_cast<float>(level) / static_cast<float>(lighting.specularLevels.size() - 1);
        lighting.specularLevels[level] = glm::mix(studioHighlight, average, roughness);
    }
    return lighting;
}

ScopedViewShadingSettings::ScopedViewShadingSettings(RenderRuntime& runtime)
    : m_runtime(runtime) {
    // Rendered frames record from the requested settings themselves.
    if (runtime.settings().viewShading == ViewShading::Rendered) {
        return;
    }
    m_requested = runtime.settings();
    runtime.setSettings(viewShadingFrameSettings(*m_requested, runtime.capabilities()));
}

ScopedViewShadingSettings::~ScopedViewShadingSettings() {
    if (m_requested) {
        m_runtime.setSettings(std::move(*m_requested));
    }
}
