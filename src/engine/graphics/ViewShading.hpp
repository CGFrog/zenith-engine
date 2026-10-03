#pragma once

#include "Rendering.hpp"

#include <array>
#include <glm/glm.hpp>
#include <optional>

// View shading modes (ViewShading in Rendering.hpp) are expressed as a reduced
// RenderSettings plus two pipeline choices, so the passes keep a single
// recording path. Everything here is CPU-only and deterministic.

// The mode a view actually draws with: Wireframe needs the wireframe
// capability and falls back to Solid without it.
ViewShading resolveViewShading(ViewShading requested, const RenderCapabilities& capabilities);

// The settings a frame records with. Rendered returns `settings` unchanged.
// Every other mode keeps the requested geometry, submission, resolution, LOD,
// anti-aliasing and tone-mapping choices and turns off:
//   - directional and local-light shadows, local lights, reflection probes,
//     the irradiance volume and image-based lighting (the studio rig of
//     studioLighting() lights the scene through the hemisphere path, with no
//     environment background);
//   - ambient occlusion, screen-space reflections and volumetric fog;
//   - bloom, motion blur, depth of field and colour grading, with exposure
//     fixed at 0 EV manual, which the studio rig is calibrated for.
// Material Preview keeps decals and particles; Solid and Wireframe drop them
// and, for Wireframe, trade TAA (whose jitter smears one-pixel edges) for FXAA.
RenderSettings viewShadingFrameSettings(const RenderSettings& settings, const RenderCapabilities& capabilities);

// True when surfaces draw the untextured clay variant of mesh.frag.
bool usesSolidSurfaceShading(ViewShading shading);

// True when triangle edges are drawn over the shaded surfaces.
bool drawsWireframeOverlay(ViewShading shading);

// True when the studio rig replaces the scene's directional and environment
// light in the camera constants.
bool usesStudioLighting(ViewShading shading);

// Fixed lights for every mode but Rendered. Radiance uses the units of
// DirectionalLightComponent::intensity and EnvironmentLightComponent colours.
struct StudioLighting {
    // World direction from a surface toward the key light.
    glm::vec3 keyDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 keyRadiance{0.0f};
    glm::vec3 skyColor{0.0f};
    glm::vec3 groundColor{0.0f};
    float diffuseIntensity{1.0f};
    float specularIntensity{1.0f};
    // Hemisphere specular radiance per roughness level, sharp (0) to rough (5),
    // as mesh.frag's hemisphere path reads environmentSpecular.
    std::array<glm::vec3, 6> specularLevels{};
};

// A camera-relative rig: the key light sits above, behind and left of the
// camera so it follows the view like a headlight without flattening the
// shading. cameraWorld is the camera-to-world transform (inverse view).
StudioLighting studioLighting(const glm::mat4& cameraWorld);

// For the duration of a frame's recording, replaces the runtime's settings
// with viewShadingFrameSettings() and restores the requested settings when it
// ends, so every pass reads one consistent reduced feature set while the
// editor and serialization keep seeing what was requested. Does nothing for
// Rendered. Interim mechanism until view shading becomes per-view state.
class ScopedViewShadingSettings {
  public:
    explicit ScopedViewShadingSettings(RenderRuntime& runtime);
    ~ScopedViewShadingSettings();
    ScopedViewShadingSettings(const ScopedViewShadingSettings&) = delete;
    ScopedViewShadingSettings& operator=(const ScopedViewShadingSettings&) = delete;

  private:
    RenderRuntime& m_runtime;
    // Set while the runtime holds the substituted frame settings.
    std::optional<RenderSettings> m_requested;
};
