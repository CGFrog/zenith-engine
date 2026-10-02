#include "engine/graphics/MeshPassFramePlan.hpp"

#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

RenderCapabilities gpuDrivenCapabilities() {
    return {.descriptorIndexing = true, .multiDrawIndirect = true, .indirectDrawCount = true, .gpuDrivenPath = true};
}

void testDefaultsFollowSettings() {
    RenderSettings settings;
    settings.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
    settings.reflections = ReflectionQuality::ScreenSpaceHalfResolution;
    const MeshPassFramePlan plan = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    expect(plan.depthPrepass, "the prepass follows RenderSettings::depthPrepass");
    expect(!plan.gpuDrivenSubmission, "CPU submission stays the default");
    expect(!plan.gpuShadowCulling, "shadow cascades cull on the CPU with CPU submission");
    expect(plan.ambientOcclusion == AmbientOcclusionQuality::HalfResolution, "AO keeps its quality with a prepass");
    expect(plan.screenSpaceReflections, "SSR traces with a prepass");
}

void testPrepassGatesScreenSpaceEffects() {
    RenderSettings settings;
    settings.ambientOcclusion = AmbientOcclusionQuality::FullResolution;
    settings.reflections = ReflectionQuality::ScreenSpaceFullResolution;
    const MeshPassFramePlan disabled =
        planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableDepthPrepass = true});
    expect(!disabled.depthPrepass, "ZENITH_DISABLE_DEPTH_PREPASS removes the prepass");
    expect(disabled.ambientOcclusion == AmbientOcclusionQuality::Off, "AO needs the prepass normals");
    expect(!disabled.screenSpaceReflections, "SSR needs the prepass normals");

    settings.depthPrepass = false;
    const MeshPassFramePlan off = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    expect(off == disabled, "the setting and the override plan the same frame");

    settings.depthPrepass = true;
    settings.reflections = ReflectionQuality::ProbesOnly;
    expect(!planMeshPassFrame(settings, gpuDrivenCapabilities(), {}).screenSpaceReflections,
           "probe-only reflections trace nothing");
}

void testGpuDrivenRequestsAndCapabilities() {
    RenderSettings settings;
    settings.preferGpuDrivenSubmission = true;
    const MeshPassFramePlan preferred = planMeshPassFrame(settings, gpuDrivenCapabilities(), {});
    expect(preferred.gpuDrivenSubmission, "the setting selects GPU-driven submission");
    expect(preferred.gpuShadowCulling, "GPU-driven submission culls shadow cascades on the GPU");

    const MeshPassFramePlan cpuShadows =
        planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableGpuShadowCulling = true});
    expect(cpuShadows.gpuDrivenSubmission && !cpuShadows.gpuShadowCulling,
           "ZENITH_DISABLE_GPU_SHADOW_CULLING keeps CPU cascade culling only");

    settings.preferGpuDrivenSubmission = false;
    expect(planMeshPassFrame(settings, gpuDrivenCapabilities(), {.forceGpuDrivenSubmission = true}).gpuDrivenSubmission,
           "ZENITH_FORCE_GPU_DRIVEN selects GPU-driven submission");
    expect(!planMeshPassFrame(settings, gpuDrivenCapabilities(), {.disableGpuShadowCulling = true}).gpuShadowCulling,
           "GPU shadow culling never runs without GPU-driven submission");

    const RenderCapabilities missing[]{
        {.descriptorIndexing = false, .multiDrawIndirect = true, .gpuDrivenPath = true},
        {.descriptorIndexing = true, .multiDrawIndirect = false, .gpuDrivenPath = true},
        {.descriptorIndexing = true, .multiDrawIndirect = true, .gpuDrivenPath = false},
    };
    for (const RenderCapabilities& capabilities : missing) {
        const MeshPassFramePlan plan = planMeshPassFrame(settings, capabilities, {.forceGpuDrivenSubmission = true});
        expect(!plan.gpuDrivenSubmission && !plan.gpuShadowCulling,
               "GPU-driven submission needs every capability, even when forced");
    }
}

void testPerPixelReflectionProbes() {
    RenderSettings settings;
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.reflectionProbes = true;
    settings.reflectionProbeBlending = ReflectionProbeBlending::PerPixel;
    expect(planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes, "per-pixel probes with IBL");
    settings.environment = EnvironmentQuality::Hemisphere;
    expect(!planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes, "hemisphere lighting has no probes");
    settings.environment = EnvironmentQuality::ImageBasedHigh;
    settings.reflectionProbeBlending = ReflectionProbeBlending::CameraOnly;
    expect(!planMeshPassFrame(settings, {}, {}).perPixelReflectionProbes, "camera-only blending binds one probe");
}
} // namespace

int main() {
    testDefaultsFollowSettings();
    testPrepassGatesScreenSpaceEffects();
    testGpuDrivenRequestsAndCapabilities();
    testPerPixelReflectionProbes();
    if (failures != 0) {
        std::cerr << failures << " mesh pass frame plan test(s) failed\n";
        return 1;
    }
    std::cout << "Mesh pass frame plan tests passed\n";
    return 0;
}
