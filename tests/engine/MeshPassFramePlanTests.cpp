#include "engine/graphics/MeshPassFramePlan.hpp"

#include "support/Doctest.hpp"

#include <iostream>

TEST_SUITE_BEGIN("MeshPassFramePlan");

namespace {

RenderCapabilities gpuDrivenCapabilities() {
    return {.descriptorIndexing = true, .multiDrawIndirect = true, .indirectDrawCount = true, .gpuDrivenPath = true};
}

TEST_CASE("MeshPassFramePlan: defaults follow settings") {
    RenderSettings settings;
    settings.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
    settings.reflections = ReflectionQuality::ScreenSpaceHalfResolution;
    const MeshPassFramePlan plan = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    CHECK_MESSAGE(plan.depthPrepass, "the prepass follows RenderSettings::depthPrepass");
    CHECK_MESSAGE(!plan.gpuDrivenSubmission, "CPU submission stays the default");
    CHECK_MESSAGE(!plan.gpuShadowCulling, "shadow cascades cull on the CPU with CPU submission");
    CHECK_MESSAGE(plan.ambientOcclusion == AmbientOcclusionQuality::HalfResolution,
                  "AO keeps its quality with a prepass");
    CHECK_MESSAGE(plan.screenSpaceReflections, "SSR traces with a prepass");
}

TEST_CASE("MeshPassFramePlan: prepass gates screen space effects") {
    RenderSettings settings;
    settings.ambientOcclusion = AmbientOcclusionQuality::FullResolution;
    settings.reflections = ReflectionQuality::ScreenSpaceFullResolution;
    const MeshPassFramePlan disabled =
        planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableDepthPrepass = true});
    CHECK_MESSAGE(!disabled.depthPrepass, "ZENITH_DISABLE_DEPTH_PREPASS removes the prepass");
    CHECK_MESSAGE(disabled.ambientOcclusion == AmbientOcclusionQuality::Off, "AO needs the prepass normals");
    CHECK_MESSAGE(!disabled.screenSpaceReflections, "SSR needs the prepass normals");

    settings.depthPrepass = false;
    const MeshPassFramePlan off = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    CHECK_MESSAGE(off == disabled, "the setting and the override plan the same frame");

    settings.depthPrepass = true;
    settings.reflections = ReflectionQuality::ProbesOnly;
    CHECK_MESSAGE(!planMeshPassFrame(settings, gpuDrivenCapabilities(), {}).screenSpaceReflections,
                  "probe-only reflections trace nothing");
}

TEST_CASE("MeshPassFramePlan: gpu driven requests and capabilities") {
    RenderSettings settings;
    settings.preferGpuDrivenSubmission = true;
    const MeshPassFramePlan preferred = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    CHECK_MESSAGE(preferred.gpuDrivenSubmission, "the setting selects GPU-driven submission");
    CHECK_MESSAGE(preferred.gpuShadowCulling, "GPU-driven submission culls shadow cascades on the GPU");

    const MeshPassFramePlan cpuShadows =
        planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableGpuShadowCulling = true});
    CHECK_MESSAGE((cpuShadows.gpuDrivenSubmission && !cpuShadows.gpuShadowCulling),
                  "ZENITH_DISABLE_GPU_SHADOW_CULLING keeps CPU cascade culling only");

    settings.preferGpuDrivenSubmission = false;
    CHECK_MESSAGE(
        planMeshPassFrame(settings, gpuDrivenCapabilities(), {.forceGpuDrivenSubmission = true}).gpuDrivenSubmission,
        "ZENITH_FORCE_GPU_DRIVEN selects GPU-driven submission");
    CHECK_MESSAGE(
        !planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableGpuShadowCulling = true}).gpuShadowCulling,
        "GPU shadow culling never runs without GPU-driven submission");

    const RenderCapabilities missing[]{
        {.descriptorIndexing = false, .multiDrawIndirect = true, .gpuDrivenPath = true},
        {.descriptorIndexing = true, .multiDrawIndirect = false, .gpuDrivenPath = true},
        {.descriptorIndexing = true, .multiDrawIndirect = true, .gpuDrivenPath = false},
    };
    for (const RenderCapabilities& capabilities : missing) {
        const MeshPassFramePlan plan = planMeshPassFrame(settings, capabilities, {.forceGpuDrivenSubmission = true});
        CHECK_MESSAGE((!plan.gpuDrivenSubmission && !plan.gpuShadowCulling),
                      "GPU-driven submission needs every capability, even when forced");
    }
}

TEST_CASE("MeshPassFramePlan: per pixel reflection probes") {
    RenderSettings settings;
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.reflectionProbes = true;
    settings.reflectionProbeBlending = ReflectionProbeBlending::PerPixel;
    CHECK_MESSAGE(planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes, "per-pixel probes with IBL");
    settings.environment = EnvironmentQuality::Hemisphere;
    CHECK_MESSAGE(!planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes, "hemisphere lighting has no probes");
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.reflectionProbeBlending = ReflectionProbeBlending::CameraOnly;
    CHECK_MESSAGE(!planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes,
                  "camera-only blending binds one probe");
}
} // namespace

TEST_SUITE_END();
