# Renderer measurements

Part of the [repository guide](../REPOSITORY_GUIDE.md). Archived history: these
are dated measurement logs that justified renderer decisions. They are kept
for reference and are not updated. The current gates are in
[Renderer validation](../RENDERER_VALIDATION.md); each feature's contract is in
[Graphics scene passes](../engine/graphics-passes.md) or
[Graphics post-processing](../engine/graphics-post.md).

## Render packet cache (2026-09-27)

Result of the [performance gate](../RENDERER_VALIDATION.md#performance) for the
render packet cache.

The 2026-09-27 Debug validation on the same machine and driver used 200 warm
measurements after the built-in 1,000-frame warmup:

| 2,000 objects | CPU median | CPU p95 | GPU median | GPU p95 | packet build |
| --- | ---: | ---: | ---: | ---: | ---: |
| cache disabled | 16.163 ms | 16.688 ms | 0.508 ms | 0.512 ms | 3.371 ms |
| cache enabled | 12.843 ms | 13.355 ms | 0.508 ms | 0.511 ms | 0.0002 ms |

The candidate improved CPU median by 20.5% and p95 by 20.0%; GPU timing stayed
within 0.3%. The default-quality regression gate therefore passed.

## Upscalers (2026-10-02)

Contract: [Upscaling](../engine/graphics-post.md#upscaling).

Release measurement (2026-10-02, showcase project, 1920x1080, Performance 2x,
`ZENITH_BENCHMARK_MODE=render`, 300 frames, medians of three runs each with
spreads under 1%):

| Upscaler | `gpu.frame` | `gpu.fsr_easu` | `gpu.fsr_rcas` | `gpu.post` |
| --- | ---: | ---: | ---: | ---: |
| Off (native, TAA) | 3.29 ms | 0 | 0 | 0.104 ms |
| TAAU | 1.54 ms | 0 | 0 | 0.064 ms |
| FSR 1 (TAA at render resolution) | 1.31 ms | 0.175 ms | 0.051 ms | 0.025 ms |

## Per-pass GPU cost on the reference hardware (2026-10-02)

The Milestone 24/25 exit measurement of the final showcase (every feature
enabled by the project's render settings: TAA, half-resolution GTAO,
full-resolution SSR, per-pixel probe blending, probe-volume indirect diffuse,
Medium volumetric fog, clustered decals, High particles, High depth of field,
Low motion blur, LUT + vignette + film grain, Medium local shadows). Release build
at `ac779a0` plus this change, RTX 2070 SUPER, VSync off,
`ZENITH_BENCHMARK_MODE=render`, `ZENITH_BENCHMARK_WARMUP_FRAMES=300`,
`ZENITH_BENCHMARK_FRAMES=600`, capture camera `0,1.7,4,0,-8` (the
`exposure_overview` view), 2026-10-02. Each configuration ran three times,
interleaved, on an otherwise idle machine; per-run medians agreed within 1%
(`gpu.frame` 4.829 / 4.864 / 4.868 ms at 1080p). Values are the median of the
three runs' medians and of their p95s, in milliseconds. The FSR 1 columns add
`ZENITH_FORCE_UPSCALER=fsr1` and `ZENITH_FORCE_UPSCALER_QUALITY=performance`
(render scale 0.5 per axis).

| Scope | 1920x1080 median / p95 | 1280x720 median / p95 | FSR 1 1920x1080 median / p95 | FSR 1 1280x720 median / p95 |
| --- | ---: | ---: | ---: | ---: |
| `gpu.shadow` (cascades cached) | 0.007 / 0.007 | 0.007 / 0.007 | 0.007 / 0.007 | 0.007 / 0.007 |
| `gpu.depth_prepass` (in opaque) | 0.188 / 0.193 | 0.094 / 0.095 | 0.063 / 0.065 | 0.040 / 0.041 |
| `gpu.depth_pyramid` (in opaque) | 0.088 / 0.090 | 0.047 / 0.048 | 0.033 / 0.035 | 0.022 / 0.023 |
| `gpu.ambient_occlusion` (in opaque) | 0.262 / 0.265 | 0.122 / 0.123 | 0.075 / 0.076 | 0.036 / 0.037 |
| `gpu.light_clusters` (in opaque) | 0.020 / 0.022 | 0.020 / 0.021 | 0.020 / 0.021 | 0.019 / 0.019 |
| `gpu.decal_clusters` (in opaque) | 0.011 / 0.012 | 0.011 / 0.012 | 0.011 / 0.012 | 0.011 / 0.012 |
| `gpu.reflection_probe_clusters` (in opaque) | 0.009 / 0.010 | 0.009 / 0.010 | 0.009 / 0.010 | 0.009 / 0.010 |
| `gpu.volumetric_fog` (in opaque) | 0.166 / 0.168 | 0.166 / 0.168 | 0.166 / 0.167 | 0.166 / 0.167 |
| `gpu.particle_simulate` (in opaque) | 0.056 / 0.058 | 0.051 / 0.052 | 0.051 / 0.052 | 0.049 / 0.051 |
| `gpu.opaque` (total) | 2.360 / 2.393 | 1.290 / 1.302 | 0.924 / 0.935 | 0.629 / 0.640 |
| opaque shading alone (total minus the nested scopes) | 1.560 | 0.770 | 0.496 | 0.277 |
| `gpu.skybox` (in blended) | 0.015 / 0.016 | 0.006 / 0.007 | 0.003 / 0.004 | 0.001 / 0.002 |
| `gpu.reflections` | 1.129 / 1.143 | 0.482 / 0.485 | 0.273 / 0.276 | 0.131 / 0.133 |
| `gpu.transmission_background` | 0.119 / 0.121 | 0.068 / 0.069 | 0.109 / 0.111 | 0.063 / 0.063 |
| `gpu.particle_draw` (in blended) | 0.068 / 0.103 | 0.017 / 0.025 | 0.012 / 0.017 | 0.008 / 0.010 |
| `gpu.blended` | 0.175 / 0.209 | 0.082 / 0.090 | 0.073 / 0.078 | 0.051 / 0.054 |
| `gpu.temporal_resolve` | 0.289 / 0.299 | 0.133 / 0.137 | 0.080 / 0.082 | 0.041 / 0.043 |
| `gpu.depth_of_field` | 0.295 / 0.302 | 0.135 / 0.137 | 0.081 / 0.082 | 0.037 / 0.038 |
| `gpu.motion_blur` (static camera) | 0.106 / 0.107 | 0.054 / 0.055 | 0.035 / 0.037 | 0.020 / 0.020 |
| `gpu.exposure` | 0.064 / 0.066 | 0.039 / 0.039 | 0.029 / 0.030 | 0.023 / 0.024 |
| `gpu.bloom_downsample` | 0.141 / 0.143 | 0.077 / 0.078 | 0.053 / 0.054 | 0.039 / 0.040 |
| `gpu.post` | 0.074 / 0.076 | 0.037 / 0.037 | 0.023 / 0.024 | 0.013 / 0.014 |
| `gpu.fsr_easu` | - | - | 0.176 / 0.178 | 0.082 / 0.082 |
| `gpu.fsr_rcas` | - | - | 0.055 / 0.055 | 0.026 / 0.027 |
| `gpu.color_grade` | 0.095 / 0.096 | 0.045 / 0.046 | 0.095 / 0.096 | 0.045 / 0.045 |
| `gpu.ui` | 0.002 / 0.002 | 0.002 / 0.002 | 0.002 / 0.002 | 0.002 / 0.002 |
| **`gpu.frame`** | **4.864 / 4.936** | **2.465 / 2.483** | **2.030 / 2.043** | **1.222 / 1.233** |

Scopes that measured at most 0.001 ms on every run (cached
`gpu.local_shadows`, `gpu.environment_precompute`, `gpu.irradiance_volume`,
`gpu.shadow_culling` and `gpu.culling`, which are GPU-driven only, and the FSR
scopes at native resolution) are omitted. `gpu.environment` is precompute plus
skybox. Nested rows are already included in their parent; see [GPU pass metrics](../RENDERER_VALIDATION.md#gpu-pass-metrics)
for the spans.

Screen-space reflections at full resolution are the largest single pass (23%
of the 1080p frame), followed by opaque shading itself (the KHR gallery,
irradiance-volume sampling, 52 clustered lights, decals and per-pixel probe
blending), the temporal resolve, depth of field and GTAO. Volumetric fog and
particle simulation are fixed costs (froxel grid and pool size) and do not
scale with resolution. Under FSR 1 everything before `gpu.post` runs at a
quarter of the pixels and the frame drops 58% at 1080p, while EASU + RCAS
(0.231 ms) and the output-resolution colour grade remain.

Against earlier showcase measurements with the content and settings of the
time: the [upscaler measurement](#upscalers-2026-10-02)
recorded 3.29 ms at 1080p (native, TAA,
full-resolution GTAO, before the gallery, decals, particles, irradiance volume,
depth of field, motion blur and grade) and the GPU-driven comparison 1.489 ms
at 720p with the project defaults. The final showcase costs 4.86 ms and
2.47 ms on its overview camera, +1.57 ms and +0.98 ms; the new post-process
passes (depth of field, motion blur, colour grade) account for 0.50 ms at
1080p, transmission, decals and particles for about 0.26 ms, and the rest is
the added gallery and probe-volume shading that the switch from full- to
half-resolution GTAO only partly offsets. Those earlier runs used other cameras
and shared the GPU, so the comparison is indicative.

## GPU-culled shadow cascades (2026-10-02)

Contract: [GPU-culled shadow cascades](../engine/graphics-passes.md#gpu-culled-shadow-cascades).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1280x720, render mode,
`ZENITH_FORCE_GPU_DRIVEN`, `ZENITH_FORCE_HIGH_QUALITY`,
`ZENITH_DISABLE_SHADOW_CACHE` so all three cascades redraw every frame, 300
warm-up and 600 measured frames) compared CPU and GPU cascade culling:

| Scene | Culling | `cpu.shadow` median | `gpu.shadow` median | `gpu.shadow_culling` median | `gpu.frame` median |
| --- | --- | ---: | ---: | ---: | ---: |
| 5,000 benchmark objects | CPU | 0.465 ms | 0.270 ms | - | 1.874 ms |
| 5,000 benchmark objects | GPU | 0.169 ms | 0.410 ms | 0.166 ms | 2.015 ms |
| showcase project (49 visible) | CPU | 0.155 ms | 0.056 ms | - | 0.858 ms |
| showcase project (49 visible) | GPU | 0.146 ms | 0.080 ms | 0.031 ms | 0.881 ms |

GPU culling moves about 0.3 ms of CPU work per fully redrawn frame at 5,000
objects onto about 0.14 ms of GPU time; cached cascades cost neither. Small
scenes gain little, so the trade only pays when the CPU is the bottleneck.

## Ground-truth ambient occlusion (2026-10-02)

Contract: [Ground-truth ambient occlusion](../engine/graphics-passes.md#ground-truth-ambient-occlusion).

The 2026-10-02 Release measurement (RTX 2070 SUPER, render mode, 200 warm-up
and 600 measured frames per run, the median of 3-5 alternating runs of each
configuration; the GPU was shared with other build and benchmark jobs, so p95s
of the dynamic scenes are noisy) compared the previous implementation (old)
with this one (new). The showcase used the `gtao_contact` camera
(`-2,1,0,0,-22`) with only `ambientOcclusion` changed:

| Scene | Resolution | Configuration | `gpu.ambient_occlusion` median / p95 | `gpu.frame` median / p95 |
| --- | --- | --- | ---: | ---: |
| showcase | 1280x720 | old Full | 0.504 / 0.511 ms | 2.295 / 2.325 ms |
| showcase | 1280x720 | old Half | 0.161 / 0.164 ms | 1.936 / 1.956 ms |
| showcase | 1280x720 | new Full | 0.514 / 0.519 ms | 2.302 / 2.331 ms |
| showcase | 1280x720 | new Half | 0.143 / 0.144 ms | 1.923 / 1.945 ms |
| showcase | 1280x720 | Off | - | 1.775 / 1.792 ms |
| showcase | 1920x1080 | old Full | 2.050 / 2.134 ms | 5.530 / 5.683 ms |
| showcase | 1920x1080 | old Half | 0.709 / 0.730 ms | 4.158 / 4.229 ms |
| showcase | 1920x1080 | new Full | 2.067 / 2.157 ms | 5.552 / 5.693 ms |
| showcase | 1920x1080 | new Half | 0.320 / 0.323 ms | 3.788 / 3.861 ms |
| showcase | 1920x1080 | Off | - | 3.473 / 3.540 ms |
| demo, `ZENITH_FORCE_HIGH_QUALITY` | 1280x720 | old (Full) | 0.313 / 0.395 ms | 1.016 / 1.124 ms |
| demo, `ZENITH_FORCE_HIGH_QUALITY` | 1280x720 | new (Half) | 0.106 / 0.119 ms | 0.808 / 0.846 ms |
| demo, `ZENITH_FORCE_HIGH_QUALITY` | 1920x1080 | old (Full) | 0.762 / 1.024 ms | 2.064 / 2.419 ms |
| demo, `ZENITH_FORCE_HIGH_QUALITY` | 1920x1080 | new (Half) | 0.229 / 0.266 ms | 1.536 / 1.672 ms |
| 4,000 objects, `ZENITH_FORCE_HIGH_QUALITY` | 1280x720 | old (Full) | 0.470 / 1.477 ms | 1.564 / 5.642 ms |
| 4,000 objects, `ZENITH_FORCE_HIGH_QUALITY` | 1280x720 | new (Half) | 0.162 / 0.567 ms | 1.378 / 4.932 ms |

Half resolution now costs 0.14 ms at 720p and 0.32 ms at 1080p, a 3.5x and
6.4x reduction from full resolution, and less than the previous half-resolution
path, which traced 3 slices against full-resolution depth and was upsampled
bilinearly in `mesh.frag`. Full resolution is unchanged within noise. The
remaining shading cost (`gpu.opaque` minus GTAO) matches Off.

Image quality at 1280x720 on the `gtao_contact` view (8-bit RMSE of the final
image): new Half differs from new Full by 0.46 (old Half from old Full: 0.30),
while ambient occlusion itself accounts for an RMSE of 5.0 against Off. Contact
shadows under the spheres and along the pedestal bases are preserved, the
depth-weighted upsample shows no halos at silhouettes, and the result is
slightly softer in the pedestals' fine surface detail. Moving the clamp after
the average changed new Full by an RMSE of 0.41 from old Full (largest channel
difference 4), within the reference-capture tolerance.

## Single-pass depth pyramid (2026-10-02)

Contract: [Single-pass depth pyramid](../engine/graphics-passes.md#single-pass-depth-pyramid).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1280x720, render mode, 300
warm-up and 600 measured frames, three alternating A/B rounds per row, medians
of the per-run medians and p95s) was taken while other jobs shared the GPU:

| Scene | Path | Build | `gpu.depth_pyramid` | `gpu.depth_prepass` | `gpu.frame` median | `gpu.frame` p95 |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| demo | CPU | per level | 0.066 ms | 0.079 ms | 0.429 ms | 0.432 ms |
| demo | CPU | single pass | 0.046 ms | 0.079 ms | 0.408 ms | 0.412 ms |
| demo | GPU-driven | per level | 0.068 ms | 0.132 ms | 0.501 ms | 0.504 ms |
| demo | GPU-driven | single pass | 0.005 ms | 0.125 ms | 0.470 ms | 0.480 ms |
| showcase | CPU | per level | 0.068 ms | 0.076 ms | 1.460 ms | 1.473 ms |
| showcase | CPU | single pass | 0.047 ms | 0.076 ms | 1.450 ms | 1.459 ms |
| showcase | GPU-driven | per level | 0.070 ms | 0.147 ms | 1.551 ms | 1.561 ms |
| showcase | GPU-driven | single pass | 0.004 ms | 0.125 ms | 1.466 ms | 1.478 ms |
| showcase, moving camera | GPU-driven | per level | 0.060 ms | 0.129 ms | 1.527 ms | 1.539 ms |
| showcase, moving camera | GPU-driven | single pass | 0.004 ms | 0.115 ms | 1.471 ms | 1.483 ms |

A full single-pass build costs about 0.047 ms against 0.066-0.070 ms for the
per-level build. The 4,000-object benchmark was too noisy under the shared load
for medians to mean much (runs of one configuration varied 3x), so it was run
five times per configuration. The fastest runs were 0.766 ms per level against
0.745 ms single pass on the CPU path, and 1.006 ms against 0.929 ms GPU-driven.
GPU-driven `gpu.depth_pyramid` fell from 0.105 ms to 0.005 ms median. No
configuration regressed beyond that noise.

## GPU-driven submission default (2026-10-02)

The decision this measurement supports is in
[Renderer validation](../RENDERER_VALIDATION.md#gpu-driven-submission-default-milestone-25).

The 2026-10-02 Release comparison (RTX 2070 SUPER, 1280x720,
`ZENITH_BENCHMARK_MODE=render`, 300 warm-up and 600 measured frames, CPU and
GPU-driven runs alternated back to back, three or more rounds) measured the
project defaults against `ZENITH_FORCE_GPU_DRIVEN=1`. Ten other build and GPU
jobs shared the machine, so the table reports the round medians of runs whose
GPU load before launch was low; contaminated rounds moved both paths by up to
10 ms and are excluded. Values are median / p95 in milliseconds.

| Scene | Path | `gpu.frame` | `cpu.opaque` | draw calls | visible |
| --- | --- | ---: | ---: | ---: | ---: |
| demo | CPU | 0.489 / 0.493 | 0.314 | 2 | 14 |
| demo | GPU-driven | 0.548 / 0.552 | 0.527 | 4 | 14 |
| showcase | CPU | 1.489 / 1.507 | 0.571 | 55 | 61 |
| showcase | GPU-driven | 1.593 / 1.617 | 0.776 | 13 | 61 |
| 4,000 benchmark objects | CPU | 0.822 / 0.841 | 0.256 | 2 | 3,376 |
| 4,000 benchmark objects | GPU-driven | 1.064 / 1.190 | 0.700 | 4 | 1,680 |
| 10,000 benchmark objects | CPU | 1.015 / 1.537 | 0.401 | 2 | 9,376 |
| 10,000 benchmark objects | GPU-driven | 1.494 / 2.259 | 0.669 | 4 | 2,014 |

GPU-driven submission is slower on the GPU in every scene (+12% on the demo,
+7% on the showcase, +29% at 4,000 objects, +47% at 10,000) and costs 0.2 to
0.4 ms more CPU recording time. Hi-Z occlusion removes up to 80% of the
benchmark instances, but those instances are cheap, instanced, and already
batched into two draws, so culling (0.11 to 0.29 ms per camera phase) and
cascade culling (0.12 to 0.30 ms) cost more than the raster work they save.
Keeping CPU cascade culling (`ZENITH_DISABLE_GPU_SHADOW_CULLING`) did not
change the outcome. `cpu.frame` in the benchmark scenes is dominated by work
outside submission (10 to 47 ms) and varied by more than the path difference
under load.

Moving the graph-shaping decisions into `MeshPassFramePlan` was checked
against the previous build on the same matrix: `gpu.frame` medians matched
within 0.01 ms (demo 0.481 to 0.478, showcase 1.483 to 1.483, 4,000 objects
0.974 to 0.940 under load), and `cpu.opaque` did not regress.

## GPU particles (2026-10-02)

Contract: [GPU particles](../engine/graphics-passes.md#gpu-particles).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1280x720, showcase project
with the High budget, `ZENITH_BENCHMARK_MODE=render`, 120 warm-up and 600
measured frames, three runs, camera `0,1.0,4.2,0,-10`) of an ember
(additive, unlit, 0.03 m) and smoke (alpha, lit, 0.25 to 1.1 m) emitter pair
whose spawn rates keep their rings full:

| Live particles | `gpu.particle_simulate` median | `gpu.particle_draw` median |
| --- | ---: | ---: |
| 10,000 (9,000 embers + 1,000 smoke) | 0.083 ms | 0.22 ms |
| 100,000 (100,000 embers) | 0.29 ms | 0.43 ms |
| 100,000 (98,000 embers + 2,000 smoke) | 0.29 ms | 0.96 to 1.43 ms |

Simulation scales with the live particles (one thread per pool slot, then a
bitonic sort over the next power of two of the live bound); the draw scales
with covered pixels, so large soft smoke costs more than many small sparks.

## Clustered decals (2026-10-02)

Contract: [Clustered decals](../engine/graphics-passes.md#clustered-decals).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1280x720, showcase project
with the decals added, `ZENITH_BENCHMARK_MODE=render`, camera
`0,1.7,4.5,0,-14`, 120 warm-up and 600 measured frames, median of three runs)
compared `decals` Off and On with five showcase-sized decals (stencil, arrow,
puddle, a normal-mapped wall plate and a corner test) and with 256 decals
(1.2 m boxes scattered over the floor; the busiest cluster touched 32 against
a capacity of 31, so one assignment was discarded):

| Scene | `decals` | `gpu.decal_clusters` | `gpu.opaque` | `gpu.frame` |
| --- | --- | ---: | ---: | ---: |
| 5 decals | Off | 0 | 1.227 ms | 2.114 ms |
| 5 decals | On | 0.017 ms | 1.265 ms | 2.152 ms |
| 256 decals | Off | 0 | 1.228 ms | 2.116 ms |
| 256 decals | On | 0.036 ms | 1.401 ms | 2.287 ms |

Assignment stays under 0.04 ms; shading (the rest of the `gpu.opaque`
difference) costs about 0.02 ms for the five decals and 0.14 ms when most floor
pixels walk a list of several decals.

## KHR material extensions (2026-10-02)

Contract: [KHR material extensions](../engine/graphics-passes.md#khr-material-extensions).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1920x1080, project render
settings with TAA, render mode, 200 warm-up and 600 measured frames, three
alternating runs, medians) put the gallery in the showcase room at (0, 0, -8.2)
and viewed it from `0,1.6,-4.5,0,-10`, where its fifteen spheres cover about
15% of the screen. The baseline is the same gallery with every extension
stripped:

| Gallery | `gpu.opaque` | `gpu.blended` | `gpu.transmission_background` | `gpu.frame` |
| --- | ---: | ---: | ---: | ---: |
| Plain metallic-roughness | 2.863 ms | 0.218 ms | - | 4.870 ms |
| With extensions | 2.849 ms | 0.292 ms | 0.119 ms | 5.022 ms |

Clearcoat, sheen, specular and ior shading are within run-to-run noise
(under 0.02 ms) in `gpu.opaque`. The three glass spheres move from opaque to
blended shading (+0.07 ms with refraction, absorption and mip sampling), and
the scene copy with its mip chain costs 0.12 ms at this resolution, paid only on
frames that draw a transmissive surface. The copy holds one RGBA16F image of
the render target's size with a full mip chain (about 22 MB at 1920x1080).

## Motion blur (2026-10-02)

Contract: [Motion blur](../engine/graphics-post.md#motion-blur).

Release measurement, 2026-10-02, RTX 2070 SUPER, showcase project with TAA,
fixed frame time, capture camera at `0,1.6,-6,0,0`, 120 warm-up and 600
measured frames (measured while other GPU work ran on the machine, so treat as
an upper bound):

| Resolution | Quality | Camera | `gpu.motion_blur` median | p95 |
| --- | --- | --- | ---: | ---: |
| 1920x1080 | High | yaw 90 deg/s (whole frame blurs) | 0.645 ms | 0.667 ms |
| 1920x1080 | Low | yaw 90 deg/s | 0.387 ms | 0.399 ms |
| 1280x720 | High | yaw 90 deg/s | 0.293 ms | 0.302 ms |
| 1920x1080 | High | static (early-out copy) | 0.105 ms | 0.106 ms |

## Depth of field (2026-10-02)

Contract: [Depth of field](../engine/graphics-post.md#depth-of-field).

The 2026-10-02 Release measurement (RTX 2070 SUPER, 1920x1080, render mode,
showcase project with TAA, camera `0,1.7,4,0,-8`, focus 4 m at f/1.0 on a 36 mm
sensor, maximum radius 16 px, 300 warm-up and 600 measured frames, three runs
each; other GPU work may have been running on the machine) gave:

| Quality | `gpu.depth_of_field` median | p95 | `gpu.frame` median |
| --- | ---: | ---: | ---: |
| Off | 0.001 ms | 0.002 ms | 3.94 ms |
| Low | 0.284 ms | 0.288 ms | 4.25 ms |
| High | 0.412 ms | 0.417 ms | 4.36 ms |
| High with FXAA (no TAA) | 0.410 ms | 0.413 ms | 3.95 ms |

The cost scales with the output area and with the square of
`maximumCocRadiusPixels`; the default 12 px radius gathers about 40% fewer
samples than the measured 16 px.

## Colour grading (2026-10-02)

Contract: [Colour grading](../engine/graphics-post.md#colour-grading).

The 2026-10-02 Release measurement (RTX 2070 SUPER, showcase project at 1920x1080, render mode,
camera `0,1.7,4,0,-8`, 300 warm-up and 600 measured frames, three runs each,
other GPU work running concurrently) gave these `gpu.color_grade` medians:

| Effects | `gpu.color_grade` median |
| --- | ---: |
| none (Off fallback; back-to-back timestamps) | 0.001 ms |
| vignette only | 0.053 ms |
| film grain only | 0.06 ms |
| 17^3 LUT only | 0.078 ms |
| LUT + vignette + grain | 0.09-0.097 ms |


Most of the cost is the extra full-screen RGBA16F write and read, so it scales
with output pixels; `gpu.post` itself is unchanged.

## First strict Vulkan validation run

Gate: [Strict Vulkan validation](../RENDERER_VALIDATION.md#strict-vulkan-validation).

The first strict run (Vulkan SDK 1.4.363, RTX 2070 SUPER) found and fixed:
push descriptors, independent blend, and demote-to-helper-invocation used
without enabling the device features; `vkCmdResetQueryPool` recorded on the
transfer-only upload queue (upload timestamps now use host query reset); and
swapchain layout transitions that did not start at the acquire semaphore's
color-attachment-output wait stage. The whole matrix now reports zero
validation and synchronization messages at 300 frames.
