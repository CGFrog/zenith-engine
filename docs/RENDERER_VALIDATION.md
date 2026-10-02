# Renderer validation

Milestone 8 keeps renderer changes measurable and capability-gated.

## Performance

`zenith_render_packet_benchmark` constructs a deterministic 2,000-object scene
and reports warmed median/p95/p99/max JSON timings for rebuild-and-sort work versus cached
packet revision checks. `RenderStats::cpuRenderPacketMilliseconds` also exposes
the live rebuild cost and whether packets were rebuilt. For whole-frame checks,
run the same executable, machine, driver, present mode, and scene twice:

```powershell
$env:ZENITH_BENCHMARK_OBJECTS='2000'
$env:ZENITH_BENCHMARK_FRAMES='500'
$env:ZENITH_DISABLE_RENDER_PACKET_CACHE='1'; build\Release\zenith.exe # baseline
Remove-Item Env:ZENITH_DISABLE_RENDER_PACKET_CACHE
build\Release\zenith.exe # candidate
```

Record median, p95, and p99 CPU/GPU values and sample counts from the JSON output. A candidate fails
the milestone gate if default-quality median or p95 regresses by more than 5%
outside run-to-run noise.

The 2026-09-27 Debug validation on the same machine and driver used 200 warm
measurements after the built-in 1,000-frame warmup:

| 2,000 objects | CPU median | CPU p95 | GPU median | GPU p95 | packet build |
| --- | ---: | ---: | ---: | ---: | ---: |
| cache disabled | 16.163 ms | 16.688 ms | 0.508 ms | 0.512 ms | 3.371 ms |
| cache enabled | 12.843 ms | 13.355 ms | 0.508 ms | 0.511 ms | 0.0002 ms |

The candidate improved CPU median by 20.5% and p95 by 20.0%; GPU timing stayed
within 0.3%. The default-quality regression gate therefore passed.

The indirect command prototype encodes the 2,000-object workload only when
descriptor indexing, multi-draw indirect, and indirect draw count are all
available. The compatible instanced path remains selected unless a same-device
benchmark improves by at least 10%. The prototype is not selected by default.

## Image stability

`zenith_render_regression` compares deterministic color-space, cascade-shadow,
edge-antialiasing, and final game-UI composition captures with sealed hashes.
The UI capture covers the post-tone-map ordering and expected output color
space. On failure it writes the candidate PPM images to the system temporary
directory for inspection.

The public anti-aliasing choices are **Off**, **FXAA**, and **TAA**
(`AntialiasingMode`). TAA is the temporal upscaling resolve
(`TemporalResolvePass`): it reprojects history with per-pixel motion vectors,
rejects disoccluded and reactive pixels, and resolves the jittered internal
resolution to the output. `zenith_temporal_quality` covers jitter, history
invalidation, and dynamic-resolution policy on the CPU. FXAA remains a cheap
fallback and still filters the linear HDR scene before tone mapping in
`post.frag`; edges are therefore judged on scene luminance rather than display
values.

## Vulkan smoke

The `zenith_editor_vulkan_smoke` CTest runs a bounded editor scenario that
resizes the offscreen viewport twice and exercises Play, Pause, Single Step,
and Stop. It is labeled `vulkan;smoke` so headless CI can exclude it while a
Vulkan-capable workstation runs it explicitly.

## Diagnostic environment variables

Every variable is read at runtime; unset means off. Presence is enough for the
`ZENITH_FORCE_*` and `ZENITH_DISABLE_*` switches, which override the requested
`RenderSettings` on every frame (`applyDiagnosticOverrides` in `Renderer.cpp`)
or at the decision point named below, after capability validation.

| Variable | Effect |
| --- | --- |
| `ZENITH_FORCE_HIGH_QUALITY` | TAA, full-resolution AO and screen-space reflections, high image-based lighting, clustered local lights, depth prepass on, cascaded (high) shadows, bloom, and High volumetric fog. |
| `ZENITH_DISABLE_VOLUMETRIC_FOG` | Forces volumetric fog Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_FORCE_TAA` | Temporal anti-aliasing. |
| `ZENITH_FORCE_DYNAMIC_RESOLUTION` | Dynamic resolution with an unreachable 0.01 ms target, so the scale drops to its minimum. |
| `ZENITH_FORCE_GPU_DRIVEN` | GPU-driven submission (two-phase occlusion culling and indirect draws) when the device supports it. |
| `ZENITH_DISABLE_DEPTH_PREPASS` | Skips the depth/normal prepass even when `RenderSettings::depthPrepass` is set. |
| `ZENITH_DISABLE_RENDER_PACKET_CACHE` | Rebuilds render packets every frame (packet-cache baseline). |
| `ZENITH_SMOKE_FRAMES=N` | Quits after N presented frames; used by bounded smoke runs and CTest. |
| `ZENITH_EDITOR_SMOKE` | Runs the scripted editor scenario (viewport resizes, play-mode transitions, streamed layer). |
| `ZENITH_BENCHMARK_FRAMES=N` | Measures N frames and prints the benchmark JSON report to stdout, then quits. |
| `ZENITH_BENCHMARK_WARMUP_FRAMES=N` | Frames rendered before measuring (default 120). |
| `ZENITH_BENCHMARK_MODE=render` | Measures rendering only (no simulation); any other value measures the full runtime. |
| `ZENITH_BENCHMARK_OBJECTS=N` | Demo scene spawns N benchmark objects (at most 10,000) and 96 local lights. |
| `ZENITH_BENCHMARK_LIGHTS=N` | Overrides the benchmark local-light count (at most 8,192); also works without objects. |
| `ZENITH_BENCHMARK_FRAME_BUDGET_MS=X` | CPU frame budget counted in `cpu.frame.over_budget`. |
| `ZENITH_BENCHMARK_SAMPLES=1` | Includes every raw sample in the report. |
| `ZENITH_FRAME_WORKERS`, `ZENITH_FRAME_GRAIN`, `ZENITH_FRAME_DETERMINISTIC` | Frame task-graph worker count, minimum grain size, and deterministic scheduling. |
| `ZENITH_RESOURCE_ROOT` | Overrides the directory assets, shaders, and derived data load from. |

### GPU pass metrics

Each frame's timestamp pool brackets every pass; `RenderStats` exposes the
retired frame's durations and the benchmark reports them as `gpu.*` metrics.
Passes a frame skips report zero. `gpu.shadow` runs from the frame start, so it
also includes `gpu.environment_precompute` (which records work only when the
environment changes), the early-phase `gpu.culling`, and any upload wait. `gpu.opaque` spans
`gpu.light_clusters`, `gpu.depth_prepass` (which includes the GPU-driven late
cull), `gpu.depth_pyramid`, `gpu.ambient_occlusion`, and shading. `gpu.blended` includes `gpu.skybox`;
`gpu.reflections` runs between them inside the scene pass and is subtracted from `gpu.blended`.
`gpu.environment` is precompute plus skybox. `gpu.volumetric_fog` (fog
injection, reprojection, and integration) runs inside `gpu.opaque` after
`gpu.light_clusters` and reports zero when fog is Off or the scene has no
`VolumetricFogComponent`; `render.volumetric_fog_bytes` reports its volumes. The editor's Graphics panel shows
the same table live.

`gpu_visibility_mismatches` counts retired GPU-driven frames whose culling kept
more objects than the CPU frustum reference. The GPU sphere test uses the
reference's exact x/y planes and tighter depth planes, so any nonzero value is a
culling defect.
