#include "MeshPassFramePlan.hpp"

#include "ViewShading.hpp"

#include <SDL3/SDL_stdinc.h>

MeshPassDiagnosticOverrides MeshPassDiagnosticOverrides::fromEnvironment() {
    return {.disableDepthPrepass = SDL_getenv("ZENITH_DISABLE_DEPTH_PREPASS") != nullptr,
            .forceGpuDrivenSubmission = SDL_getenv("ZENITH_FORCE_GPU_DRIVEN") != nullptr,
            .disableGpuShadowCulling = SDL_getenv("ZENITH_DISABLE_GPU_SHADOW_CULLING") != nullptr};
}

MeshPassFramePlan planMeshPassFrame(const RenderSettings& settings, const RenderCapabilities& capabilities,
                                    const MeshPassDiagnosticOverrides& overrides) {
    MeshPassFramePlan plan;
    // Wireframe draws edges only and through every surface, so it draws no
    // surface depth and culls nothing by occlusion.
    const bool surfaces = drawsSurfaces(settings.viewShading);
    plan.depthPrepass = surfaces && settings.depthPrepass && !overrides.disableDepthPrepass;
    const bool gpuDrivenAvailable =
        capabilities.gpuDrivenPath && capabilities.descriptorIndexing && capabilities.multiDrawIndirect;
    const bool gpuDrivenRequested = settings.preferGpuDrivenSubmission || overrides.forceGpuDrivenSubmission;
    plan.gpuDrivenSubmission = surfaces && gpuDrivenAvailable && gpuDrivenRequested;
    plan.gpuShadowCulling = plan.gpuDrivenSubmission && !overrides.disableGpuShadowCulling;
    plan.ambientOcclusion = plan.depthPrepass ? settings.ambientOcclusion : AmbientOcclusionQuality::Off;
    plan.screenSpaceReflections = plan.depthPrepass && settings.reflections != ReflectionQuality::ProbesOnly;
    plan.perPixelReflectionProbes = settings.environment != EnvironmentQuality::Hemisphere &&
                                    settings.reflectionProbes &&
                                    settings.reflectionProbeBlending == ReflectionProbeBlending::PerPixel;
    return plan;
}
