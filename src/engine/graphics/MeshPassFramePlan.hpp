#pragma once

// Per-frame, per-view decisions MeshPass records with (prepass, GPU-driven, AO, SSR, probes).

#include "Rendering.hpp"

// Diagnostic switches that override RenderSettings for A/B measurement. The
// renderer reads them once from the process environment at startup
// (docs/RENDERER_VALIDATION.md lists the variables).
struct MeshPassDiagnosticOverrides {
    // ZENITH_DISABLE_DEPTH_PREPASS
    bool disableDepthPrepass{false};
    // ZENITH_FORCE_GPU_DRIVEN
    bool forceGpuDrivenSubmission{false};
    // ZENITH_DISABLE_GPU_SHADOW_CULLING
    bool disableGpuShadowCulling{false};

    static MeshPassDiagnosticOverrides fromEnvironment();
};

// The per-frame decisions MeshPass::record acts on that also change which
// passes the frame graph contains. MeshPass records from this plan and the
// Renderer derives RenderGraphShape from the same value, so the descriptive
// graph cannot drift from what is recorded. A pass whose recording decision
// changes the graph adds its field here and sets it in planMeshPassFrame().
struct MeshPassFramePlan {
    bool depthPrepass{false};
    // Two-phase GPU occlusion culling and indirect draws for opaque and masked
    // surfaces; blended surfaces and outlines stay CPU-submitted.
    bool gpuDrivenSubmission{false};
    // Dirty shadow cascades cull on the GPU; requires gpuDrivenSubmission.
    bool gpuShadowCulling{false};
    // GTAO needs the prepass normals, so it is Off without a prepass.
    AmbientOcclusionQuality ambientOcclusion{AmbientOcclusionQuality::Off};
    // Traced from the prepass normals, specular, and depth pyramid.
    bool screenSpaceReflections{false};
    bool perPixelReflectionProbes{false};

    bool operator==(const MeshPassFramePlan&) const = default;
};

// Pure function of its inputs. `settings` are the validated settings, so
// `capabilities` only gates paths validation does not already fall back from.
MeshPassFramePlan planMeshPassFrame(const RenderSettings& settings, const RenderCapabilities& capabilities,
                                    const MeshPassDiagnosticOverrides& overrides);
