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

## Upscaling

`RenderSettings::upscaler` selects how a frame rendered below output resolution
reaches it (`UpscalerMode`, serialized as `upscaler.mode`):

| Mode | Value | Behavior |
| --- | ---: | --- |
| Off | 0 | Historical behavior and the fallback: the internal resolution follows `dynamicResolution` alone, and TAA upsamples whatever it renders. Older documents without the key load as Off, so `antialiasing` 2 with dynamic resolution still means TAAU. |
| TAAU | 1 | Renders at the preset's scale and reconstructs with `TemporalResolvePass`. Requires TAA; validation enables it. |
| FSR 1 | 2 | Renders at the preset's scale, anti-aliases at render resolution (FXAA, or TAA resolving at render resolution), exposes, blooms, and tone maps there, then upsamples with AMD FidelityFX Super Resolution 1 EASU and sharpens with RCAS (`SpatialUpscalePass`). |

`upscaler.quality` picks the per-axis ratio: Quality 1.5x (0), Balanced 1.7x
(1), Performance 2x (2). `upscaler.sharpness` in [0, 1] sets RCAS: 1 is AMD's
maximum (0 stops) and each 0.5 below it halves the sharpening (default 0.9,
AMD's suggested 0.2 stops). With dynamic resolution enabled, its scale range
applies relative to the preset (both bounds are multiplied by the preset's
render scale), so the preset is the unloaded resolution; the internal extent
still varies inside fixed output-sized targets.

FSR 1 filters perceptual values, so it runs after tone mapping: `post.frag`
composites into the top-left render extent of an output-sized input target in
the output format (sRGB, which stores display values with 8-bit perceptual
precision), `fsr_easu.comp` gamma-2 encodes the display-linear values it reads,
upsamples, and stores them in an RGBA8 image, and `fsr_rcas.frag` sharpens that
encoding and decodes it in the fullscreen draw into the output. RCAS is a
fragment pass rather than compute so the swapchain needs no storage usage. HDR
values never reach EASU, so bright highlights cannot ring. The EASU taps are
fetched individually and clamped to the render extent, because texels beyond
it are stale under dynamic resolution. The editor grid composites at render
resolution and is upscaled with the scene.

The capability check (`RenderCapabilities::spatialUpscaling`) requires RGBA8
storage images and a sampled, renderable output format. Without it validation
falls back to TAAU where TAA is available and to native resolution otherwise,
each with a `fallbacks` message. A vendor upscaler such as DLSS would be
another `UpscalerMode` behind its own capability check; it needs a proprietary
SDK and is not included.

GPU scopes are `gpu.fsr_easu` (timestamps 47/48) and `gpu.fsr_rcas` (49/50);
`gpu.spatial_upscale` is their sum, checked against
`RenderPassBudgets::spatialUpscale` (serialized as `spatialUpscale`).
`render.spatial_upscale_bytes` reports the two targets once allocated. Both
scopes report zero unless the frame used FSR 1. `ZENITH_FORCE_UPSCALER` and
`ZENITH_FORCE_UPSCALER_QUALITY` select a mode and preset for diagnostics, and
`zenith_spatial_upscale` covers the presets, EASU constants and reconstruction,
RCAS, serialization, and fallbacks on the CPU.

Release measurement (2026-10-02, showcase project, 1920x1080, Performance 2x,
`ZENITH_BENCHMARK_MODE=render`, 300 frames, medians of three runs each with
spreads under 1%):

| Upscaler | `gpu.frame` | `gpu.fsr_easu` | `gpu.fsr_rcas` | `gpu.post` |
| --- | ---: | ---: | ---: | ---: |
| Off (native, TAA) | 3.29 ms | 0 | 0 | 0.104 ms |
| TAAU | 1.54 ms | 0 | 0 | 0.064 ms |
| FSR 1 (TAA at render resolution) | 1.31 ms | 0.175 ms | 0.051 ms | 0.025 ms |

## Frame capture and reference images

Visual checks do not need RenderDoc or window focus. `ZENITH_CAPTURE_FRAME=N`
with `ZENITH_CAPTURE_PATH=file.png` copies the swapchain image of the Nth
frame, after every pass including UI, to an 8-bit RGBA PNG (sRGB-encoded, as
presented) and ends the run with that frame unless `ZENITH_SMOKE_FRAMES` asks for
more. The swapchain is then created with `TRANSFER_SRC` usage; the copy waits for
that one frame before presenting it. A capture also fixes the frame time (see
`ZENITH_FIXED_FRAME_TIME`) and fails, rather than writing a misleading image, if
assets were still uploading on the captured frame.

The demo game layer adds a scripted camera for repeatable views:
`ZENITH_CAPTURE_CAMERA=x,y,z,yaw,pitch` replaces the player with a camera at that
world position (meters; yaw turns left from looking down -Z, pitch looks up, both
in degrees), and `ZENITH_CAPTURE_CAMERA_MOTION=vx,vy,vz,yawRate,pitchRate` moves
it linearly per second of simulated time. Input, menus, the HUD, and the persisted
game settings are ignored, so the project's render settings apply unchanged.

```powershell
$env:ZENITH_WINDOW_SIZE='640x360'
$env:ZENITH_CAPTURE_CAMERA='0,1.7,4,0,-8'
$env:ZENITH_CAPTURE_FRAME='300'
$env:ZENITH_CAPTURE_PATH='build\captures\overview.png'
build\Debug\zenith.exe --project projects\showcase.zenith-project
```

### Reference sets

Each folder under `tests/references/` is one reference set: a `captures.json`
naming the project it renders, plus the reference PNGs.

- `showcase/` covers converged and adapting auto exposure, TAAU ghosting during
  a lateral pan, alpha-tested foliage edges through the depth prepass, GTAO
  contact occlusion, screen-space reflections, and bloom.
- `irradiance_room/` covers volumetric fog light shafts through the room's
  window (dense uniform fog, ambient-lit by the baked irradiance volume) and
  the volume's bounce lighting and color bleed.

Each view
names a capture frame, a camera pose and optional motion, optional extra
environment variables, and optional per-view tolerances. The checked-in PNGs
beside it are the references.

`zenith_reference_capture` renders every view with `zenith`, a clean
environment (benchmark, smoke, and `ZENITH_FORCE_*` switches removed), the
manifest's window size, and `ZENITH_FRAME_DETERMINISTIC=1`, then compares each
capture with its reference in `engine/diagnostics/ImageComparison`. Differences
are per RGB channel in 8-bit units: a pixel differs when any channel exceeds
`channelTolerance`, and a view fails when the RMSE, the largest channel
difference, or the fraction of differing pixels exceeds its limit. A failure
writes `<view>.diff.png` (amplified absolute difference) beside the capture in
`build/captures/<set>/`. Consecutive runs on one machine differ by at most a
few 8-bit steps in a handful of pixels; the default limits (3 per channel, RMSE
1.5, 0.1% of pixels) leave room for that and nothing larger.

The CTests `zenith_<set>_reference_captures` (labels `vulkan;capture`) run the
comparison and return 77, reported as skipped, when the machine has no
hardware Vulkan device. CI runs it in the Vulkan-labelled step only.

**Regenerate the references** after an intentional visual change, a content
change, or a driver/GPU change, then review the PNG diff before committing.
`zenith_update_references` regenerates every set; `zenith_update_<set>_references`
regenerates one:

```powershell
cmake --build build --config Debug --target zenith_update_references
```

To check stability, or to work on one view, run the driver directly;
`--repeat 2` captures each view twice and compares the runs with each other
before comparing with the reference:

```powershell
build\Debug\zenith_reference_capture.exe --executable build\Debug\zenith.exe `
    --manifest tests\references\showcase\captures.json --output build\captures\showcase `
    --repeat 2 --view taau_ghosting
```

References depend on the GPU and driver that produced them; the current set was
captured on the development workstation's GPU. A runner with a different GPU
should regenerate its own set rather than loosen the tolerances.

## Vulkan smoke

The `zenith_editor_vulkan_smoke` CTest runs a bounded editor scenario that
resizes the offscreen viewport twice and exercises Play, Pause, Single Step,
and Stop. It is labeled `vulkan;smoke` so headless CI can exclude it while a
Vulkan-capable workstation runs it explicitly.

## Strict Vulkan validation

`VulkanContext` enables `VK_LAYER_KHRONOS_validation` whenever it is
installed and counts every message the debug messenger receives (through the
messenger's `pUserData`, never global state). `Engine::graphicsValidation()`
returns the counts as a `GraphicsValidationReport`, including messages emitted
while the device and instance are destroyed. Executables print a one-line
summary on exit when the layer was active.

Setting `ZENITH_VALIDATION_STRICT=1` turns validation into a gate:

- synchronization validation is enabled through `VkValidationFeaturesEXT`;
- the process exits with code 77 when the Vulkan loader or the validation
  layer is unavailable (CTest reports the test as skipped);
- the process exits nonzero if any error, or any warning of the validation or
  general type, was reported.

Two kinds of message are printed and counted separately but never fail a
strict run:

- performance warnings (`VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT`):
  advisory, driver-dependent hints rather than API-usage violations
  (best-practices validation is not enabled);
- Vulkan loader diagnostics (message ID `Loader Message`): they describe the
  machine's layer registry, such as implicit-layer manifests left behind by
  uninstalled overlays, not the application.

No validation message IDs are filtered. A confirmed layer false positive must
be filtered by its ID in `VulkanContext::debugCallback` with a comment naming
the layer issue.

The `vulkan;validation` CTest label runs every combination of

| Axis | Values |
| --- | --- |
| Content | demo (`zenith`), showcase (`zenith --project projects/showcase.zenith-project`) |
| Submission | CPU, `ZENITH_FORCE_GPU_DRIVEN=1` |
| Quality | project default, `ZENITH_FORCE_HIGH_QUALITY=1` + `ZENITH_FORCE_DYNAMIC_RESOLUTION=1`, the same plus `ZENITH_FORCE_UPSCALER=fsr1` + `ZENITH_FORCE_UPSCALER_QUALITY=performance` (`fsr1_dynamic`) |

for 120 frames each, plus the scripted editor smoke
(`zenith_validation_editor`), all with `ZENITH_VALIDATION_STRICT=1`:

```powershell
ctest --test-dir build -C Debug --output-on-failure -L validation
```

The first strict run (Vulkan SDK 1.4.363, RTX 2070 SUPER) found and fixed:
push descriptors, independent blend, and demote-to-helper-invocation used
without enabling the device features; `vkCmdResetQueryPool` recorded on the
transfer-only upload queue (upload timestamps now use host query reset); and
swapchain layout transitions that did not start at the acquire semaphore's
color-attachment-output wait stage. The whole matrix now reports zero
validation and synchronization messages at 300 frames.

CI excludes every `vulkan` test from the CPU step and runs the validation
matrix in a separate, non-blocking Windows step that skips without the layer.

## Diagnostic environment variables

Every variable is read at runtime; unset means off. Presence is enough for the
`ZENITH_FORCE_*` and `ZENITH_DISABLE_*` switches, which override the requested
`RenderSettings` on every frame (`applyDiagnosticOverrides` in `Renderer.cpp`)
or at the decision point named below, after capability validation.
`ZENITH_DISABLE_DEPTH_PREPASS`, `ZENITH_FORCE_GPU_DRIVEN`, and
`ZENITH_DISABLE_GPU_SHADOW_CULLING` are read once when the renderer starts
(`MeshPassDiagnosticOverrides`) and applied by `planMeshPassFrame`, the one
place both the recorded mesh pass and the render-graph model take those
decisions from.

| Variable | Effect |
| --- | --- |
| `ZENITH_FORCE_HIGH_QUALITY` | TAA, half-resolution AO (the recommended quality; see [Ground-truth ambient occlusion](#ground-truth-ambient-occlusion)), full-resolution screen-space reflections, high image-based lighting, per-pixel reflection probe blending, probe-volume indirect diffuse, clustered local lights with high-quality local shadows, depth prepass on, cascaded (high) shadows, bloom, High volumetric fog, High depth of field, and High motion blur. |
| `ZENITH_DISABLE_VOLUMETRIC_FOG` | Forces volumetric fog Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_DISABLE_DEPTH_OF_FIELD` | Forces depth of field Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_DISABLE_MOTION_BLUR` | Forces motion blur Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_FORCE_TAA` | Temporal anti-aliasing. |
| `ZENITH_FORCE_DYNAMIC_RESOLUTION` | Dynamic resolution with an unreachable 0.01 ms target, so the scale drops to its minimum. |
| `ZENITH_FORCE_UPSCALER=off\|taau\|fsr1` | Sets `RenderSettings::upscaler.mode` (`native` is an alias of `off`); `taau` also forces TAA. Unknown values are ignored. Forced FSR 1 on a device without support renders at native resolution. |
| `ZENITH_FORCE_UPSCALER_QUALITY=quality\|balanced\|performance` | Sets the upscaler preset (1.5x, 1.7x, 2x per axis). |
| `ZENITH_FORCE_GPU_DRIVEN` | GPU-driven submission (two-phase occlusion culling and indirect draws) when the device supports it. Shadow cascades are then culled on the GPU too. |
| `ZENITH_DISABLE_GPU_SHADOW_CULLING` | Keeps CPU shadow-cascade culling while GPU-driven submission is active (A/B baseline). |
| `ZENITH_DISABLE_SHADOW_CACHE` | Redraws every shadow cascade each frame, so shadow culling and rendering can be measured on a static scene. |
| `ZENITH_DISABLE_DEPTH_PREPASS` | Skips the depth/normal prepass even when `RenderSettings::depthPrepass` is set. |
| `ZENITH_DISABLE_RENDER_PACKET_CACHE` | Rebuilds render packets every frame (packet-cache baseline). |
| `ZENITH_DEPTH_PYRAMID_PER_LEVEL` | Builds the depth pyramid with one dispatch per level and always rebuilds it after the prepass (the pre-Milestone 25 behavior; A/B baseline for the single-pass build). |
| `ZENITH_VALIDATION_STRICT=1` | Requires the validation layer, enables synchronization validation, and fails the process on any counted validation message (exit 77 when the layer is unavailable). Any value other than empty or `0` enables it. |
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
| `ZENITH_CAPTURE_FRAME=N`, `ZENITH_CAPTURE_PATH=file.png` | Writes the presented image of frame N (1-based) to a PNG and ends the run there unless `ZENITH_SMOKE_FRAMES` is larger. Both are required together. |
| `ZENITH_FIXED_FRAME_TIME=1` | Advances simulation, animation, and auto-exposure adaptation by the physics fixed step each frame instead of wall-clock time. Implied by a capture. |
| `ZENITH_WINDOW_SIZE=WxH` | Initial window size (16 to 16,384 per side); equals the capture size without display scaling. |
| `ZENITH_CAPTURE_CAMERA=x,y,z,yaw,pitch` | Demo game: replaces the player with a fixed camera and ignores input, menus, the HUD, and persisted game settings. |
| `ZENITH_CAPTURE_CAMERA_MOTION=vx,vy,vz,yawRate,pitchRate` | Demo game: moves the capture camera linearly per simulated second. |
| `ZENITH_RESOURCE_ROOT` | Overrides the directory assets, shaders, and derived data load from. |

### GPU pass metrics

Each frame's timestamp pool brackets every pass; `RenderStats` exposes the
retired frame's durations and the benchmark reports them as `gpu.*` metrics.
Passes a frame skips report zero. `gpu.shadow` runs from the frame start, so it
also includes `gpu.environment_precompute` (which records work only when the
environment changes or a reflection probe becomes resident), `gpu.local_shadows`
(zero on frames that reuse every cached atlas tile), `gpu.shadow_culling`
(GPU-driven only; zero on frames whose cascades were all cached), the
early-phase `gpu.culling`, and any upload wait. `gpu.opaque` spans
`gpu.light_clusters`, `gpu.reflection_probe_clusters`, `gpu.depth_prepass` (which includes the GPU-driven late
cull), `gpu.depth_pyramid`, `gpu.ambient_occlusion`, and shading. `gpu.blended` includes `gpu.skybox`;
`gpu.reflections` runs between them inside the scene pass and is subtracted from `gpu.blended`.
`gpu.environment` is precompute plus skybox. `gpu.volumetric_fog` (fog
injection, reprojection, and integration) runs inside `gpu.opaque` after
`gpu.light_clusters` and reports zero when fog is Off or the scene has no
`VolumetricFogComponent`; `render.volumetric_fog_bytes` reports its volumes.
`gpu.irradiance_volume` is the
irradiance-volume probe upload, which records work only on frames whose bound
volume changed and also falls inside `gpu.shadow`; sampling the volume is part
of opaque and blended shading, so its per-pixel cost shows up in `gpu.opaque`
(compare against `indirectDiffuse` set to Environment, the Off fallback).
`gpu.motion_blur` runs after `gpu.temporal_resolve` and before `gpu.exposure`
and reports zero when motion blur is Off. The editor's Graphics panel shows the
same table live.
`gpu.color_grade` is the colour-grade draw after `gpu.post` (zero when every
grade effect is off; see "Colour grading"). The editor's Graphics panel shows
the same table live.

### GPU-culled shadow cascades

GPU-driven submission culls each dirty directional shadow cascade on the GPU
(`GpuDrivenPass::cullFrustumView`) and draws it with indirect commands. There is
no separate render setting: the technique's Off fallback is the CPU cascade
cull, used whenever GPU-driven submission is off or unsupported, and
`ZENITH_DISABLE_GPU_SHADOW_CULLING` forces it for A/B measurement. Culling has its
own timestamp scope (`gpu.shadow_culling`) and budget
(`RenderPassBudgets::shadowCulling`, serialized as `shadowCulling`). The three
cascade regions add about 33 MB of device memory to the GPU-driven buffers
(visible instances, counters, and commands sized like a camera phase). With GPU
culling, `RenderStats::shadowCasterCount` reports the casters a retired frame
emitted, like `visibleCount`, and shadow triangles are not counted.

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

## Ground-truth ambient occlusion

`RenderSettings::ambientOcclusion` selects GTAO's resolution and sampling
pattern (`ambient_occlusion::qualityProfile`); both qualities write a
render-resolution R8 image, so shading is identical:

| Quality | Traced texels | Slices x steps per side | Horizon depth | Denoise |
| --- | --- | --- | --- | --- |
| **HalfResolution** (recommended) | one per 2x2 pixels | 2 x 4 | pyramid level 1, farthest/closest depth in a checkerboard | `gtao_upsample.comp`: 4x4 raw texels per render pixel, depth-weighted against the pixel's own view depth |
| **FullResolution** | every pixel | 3 x 4 | pyramid level 0 | `gtao_denoise.comp`: depth-aware 5x5 |

Both rotate slices with a 4x4 ordered dither (advanced per frame under TAA),
and the denoise window holds each of its 16 phases, so half resolution still
averages 32 slice directions per pixel. The clamp to [0, 1] and the occlusion
power apply after the average: clamping each few-slice estimate first darkened
open surfaces far from the view axis. The scope is `gpu.ambient_occlusion` and
the budget `RenderPassBudgets::ambientOcclusion` (zero, unenforced, like every
default budget; half resolution fits 0.2 ms at 720p and 0.35 ms at 1080p on the
reference GPU).

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
### Single-pass depth pyramid

`DepthPyramid` builds every level in one dispatch
(`shaders/depth_pyramid_single_pass.comp`, structured after AMD FidelityFX SPD):
each 256-thread workgroup reduces a 64x64 tile through six levels in shared
memory, and a global atomic counter elects the last workgroup, which completes
the edge texels of levels whose extent does not tile the base exactly and then
reduces the remaining levels. The result is identical to the per-level build
(`zenith_depth_prepass` checks both against a brute-force min/max on odd
sizes). Devices without `shaderStorageImageArrayDynamicIndexing` fall back to
the per-level build and log it once; `ZENITH_DEPTH_PYRAMID_PER_LEVEL` forces it.

With GPU-driven submission and the depth prepass, the pyramid used to be built
twice per frame: once from the early phase's depth for the late cull, and again
after the late phase for screen-space effects. The second build now passes the
late phase's emitted-instance count (`GpuDrivenPass::lateEmittedCount`) and the
shader exits when it is zero, since the depth is then unchanged. In the steady
state the pyramid is built once per frame; when objects become visible the
rebuild still runs, so SSR and GTAO always see the complete depth. Using last
frame's pyramid for the late cull instead would cull objects disoccluded by
camera motion for a frame. `gpu.depth_pyramid` measures only the post-prepass
build on that path (the occlusion build counts toward `gpu.depth_prepass`).

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

### GPU-driven submission default (Milestone 25)

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

Decision: CPU (instanced) submission stays the default and there is no
automatic threshold, because no measured draw or instance count produced a
GPU-frame win. GPU-driven submission stays opt-in through
`RenderSettings::preferGpuDrivenSubmission` or `ZENITH_FORCE_GPU_DRIVEN`, and
remains covered by the validation matrix. Revisit the decision with a
vertex- or fill-bound occluded scene (many distinct meshes or materials,
expensive shading) where CPU draw counts grow with visible batches.

Moving the graph-shaping decisions into `MeshPassFramePlan` was checked
against the previous build on the same matrix: `gpu.frame` medians matched
within 0.01 ms (demo 0.481 to 0.478, showcase 1.483 to 1.483, 4,000 objects
0.974 to 0.940 under load), and `cpu.opaque` did not regress.

## Reflection probe blending

`RenderSettings::reflectionProbeBlending` selects how reflection probes reach
shading when image-based lighting and `reflectionProbes` are on:

- **PerPixel** (default): up to eight probes nearest the camera stay resident.
  `EnvironmentPass` copies each one's prefiltered specular cube into a slot of
  one cube-map array (binding 16) once, when it becomes resident, building at
  most two new probe environments per frame. `ReflectionProbeClusterPass`
  uploads the probe records and their irradiance and
  `reflection_probe_clusters.comp` assigns each probe's influence box (its box
  expanded by the blend distance) to the light-cluster grid (binding 17).
  `mesh.frag` blends up to four of its cluster's probes per pixel with
  box-projected specular lookups and blended irradiance, matching
  `blendReflectionProbes`; the scene environment fills the remainder across
  each probe's blend distance. The GPU scope is `gpu.reflection_probe_clusters`
  with the `reflectionProbeClusters` budget; `reflection_probes` counts the
  resident probes.
- **CameraOnly** (the Off fallback): the dominant probe at the camera is
  blended over the whole frame, as before. Validation selects it, with a
  fallback message, when the device lacks per-pixel support.

`zenith_reflection_probes` covers box projection against an independent slab
intersection, weight normalization against `blendReflectionProbes`, resident
selection, and conservative cluster assignment against brute-force frustum
samples. The showcase project's west and east halves carry two overlapping
probes with distinct environments (`tools/showcase/build_probe_environments.py`).

`gpu_visibility_mismatches` counts retired GPU-driven frames whose culling kept
more objects than the CPU frustum reference. The GPU sphere test uses the
reference's exact x/y planes and tighter depth planes, so any nonzero value is a
culling defect.

## Silent-fallback audit

Renderer paths that keep a frame alive by substituting or dropping content must
be observable. Upload deferrals are counted in `RenderStats` and exported by the
benchmark; asset-level failures are logged once rather than every frame.

| Fallback | Observable as |
| --- | --- |
| Mesh upload deferred (staging backpressure, geometry arena full); its draws are skipped | `RenderStats::deferredMeshUploads`, `upload.deferred_meshes` |
| Texture upload deferred (staging backpressure, bindless table full); materials sample the white/flat-normal fallback slot | `RenderStats::deferredTextureUploads`, `upload.deferred_textures` |
| Any mesh or texture deferred for more than `RenderSettings::deferredUploadWarningFrames` consecutive frames (default 120; zero disables) | One warning per asset per deferral episode (`DeferredUploadTracker`) |
| Texture that can never upload (BC on a device without BC sampling, invalid BC data, RGBA8 size mismatch, larger than one 64 MiB staging slot) | Logged once, then skipped until the asset unloads; samples the fallback |
| Mesh larger than one staging slot | Logged once, then skipped until the asset unloads (previously retried every frame) |
| Geometry arena or bindless texture table full | Logged once per episode (reset when an allocation succeeds), and counted as a deferral |
| Scene local lights over the effective local-light limit (nearest kept) | `RenderStats::droppedLocalLights`, `lights.dropped` |
| Cluster light assignments over the per-cluster capacity | `RenderStats::discardedLightAssignments` (existing) |
| Staging-ring backpressure | `RenderStats::uploadBackpressureEvents` (existing) |
| Unsupported capabilities (IBL, probes, TAA, SSAO, SSR, GPU-driven, local lights) | `ValidatedRenderSettings::fallbacks`, reported by the application (existing) |
| Environment map that cannot be built; ambient occlusion formats unsupported | Logged once (existing) |

`DeferredUploadTracker` ages an asset once per frame however many draws request
it, forgets it when its upload lands or when a frame passes without a request
(released or no longer drawn), so its memory is bounded by the assets deferred
in the current frame. While a mesh is deferred, render packets are rebuilt every
frame so the cached-packet path retries the upload; before this audit a mesh
deferred on the last packet rebuild stayed invisible until an unrelated scene or
asset revision forced another rebuild.

Both counters are briefly nonzero while a large scene streams in and must be
zero at steady state; a nonzero steady-state value means content is missing.

Not changed by this audit, because they fail the whole frame loudly rather than
degrading silently: renderables over `maximumObjectsPerFrame`, skin palettes
over the per-frame matrix capacity, materials over the material capacity, and
GPU-driven instance/group overflows. Each logs every frame it occurs. An
environment map texture without usable pixels still falls back to the analytic
sky without a message.

### Motion blur

`RenderSettings::motionBlur` (`MotionBlurQuality`: Off, Low, High; JSON
`motionBlur` 0-2) blurs the HDR scene by the existing motion vectors after the
temporal resolve and before exposure, bloom, and tone mapping, so camera and
object motion (including skinned meshes) blur alike. Off is the default and the
fallback: no work is recorded, no images are allocated, and the post pass reads
the resolved (or raw HDR) scene as before; validation also selects it, with a
fallback message, when `RenderCapabilities::motionBlur` is false.
`motionBlurShutterFraction` (0-1, default 0.5) is the exposure as a fraction of
a 60 Hz frame; velocities are rescaled by the real frame time
(`motion_blur::velocityScale`), so the blur length follows on-screen speed, not
the frame rate. `motionBlurMaximumRadiusPixels` (1-64 output pixels, default 32)
caps the blur on either side of a pixel and sets the tile size. All three keys
are optional in the render-settings document.

The pass reduces blur velocities to tiles (`motion_blur_tile_max.comp`), takes
each tile's 3x3 neighbourhood maximum (`motion_blur_neighbor_max.comp`), and
gathers 8 (Low) or 16 (High) depth-weighted samples along it
(`motion_blur_reconstruct.comp`); pixels whose neighbourhood moves less than
half a pixel are copied. The scope is `gpu.motion_blur` (timestamps 43-44) with
the `motionBlur` budget; `render.motion_blur_bytes` reports the blurred scene
and tile images (17 MB at 1920x1080). `zenith_motion_blur` covers velocity
scaling, radius and tile math, and the settings round trip and fallback.

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

### Local-light shadows

`RenderSettings::localShadows` (`LocalShadowQuality`) shadows point and spot
lights whose components set `castsShadows`. Off is the fallback: lights stay
unshadowed, a 64x64 placeholder atlas stays bound, and no tile pass runs.
Validation also turns it Off when clustered local lighting is disabled or
unsupported. Low, Medium, and High pick the atlas size (2048 or 4096 texels),
the default tile sizes, the number of shadowed lights (8, 16, 24), and the
per-frame tile budget (6, 12, 24 tiles); see `localShadowProfile`.

Tiles keep their depth across frames and are redrawn only when their light
moves, a caster within the light's range moves or animates, or the caster set
or materials change; dirty tiles beyond the budget keep shading with their
previous depth and projection. The benchmark reports `gpu.local_shadows` and
`cpu.local_shadows` against `RenderPassBudgets::localShadows`, plus
`local_shadows.tiles_rendered`, `tiles_cached`, `tiles_pending`,
`lights_rejected` (no atlas space), and `render.local_shadow_atlas_bytes`.
`zenith_local_shadows` covers request order, cache invalidation, budget
selection, and the face projections on the CPU.

## Depth of field

`RenderSettings::depthOfField` (`DepthOfFieldSettings`, serialized as the
optional `depthOfField` object) blurs the HDR scene with a thin-lens circle of
confusion, after the temporal resolve and before exposure, bloom, and tone
mapping. `quality` (`DepthOfFieldQuality`) is Off (the fallback: no work, no
memory), Low, or High; both gather at half resolution, and Low spaces its
golden-angle spiral twice as far apart (about half the samples). The lens is
`focusDistance` (meters along the view axis), `fStop`, `sensorHeightMillimeters`
(with the camera's vertical field of view it fixes the focal length), and
`maximumCocRadiusPixels` (the blur radius limit in pixels of the blurred image,
which bounds the gather cost). Deserialization rejects values outside the
ranges in `DepthOfField.hpp`; validation turns depth of field Off with a
fallback message when the device lacks the capability or settings set in code
are out of range. `ZENITH_FORCE_HIGH_QUALITY` selects High and
`ZENITH_DISABLE_DEPTH_OF_FIELD` forces Off.

The GPU scope is `gpu.depth_of_field` (timestamps 41 and 42, zero when Off)
against `RenderPassBudgets::depthOfField` (`depthOfField` in the budgets
object); `render.depth_of_field_bytes` reports its targets (two half-resolution
and one output-resolution RGBA16F image, allocated on first use).
`zenith_depth_of_field` covers the CoC against the thin-lens formula, depth
linearization, the gather sample count, the composite blend, settings
serialization, and the fallbacks.

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
## Colour grading

`ColorGradePass` is a display-referred finishing stage after tone mapping. Its
three effects are independent `RenderSettings` members, each Off by default,
serialized under the render-settings `colorGrade` object (every object and
field optional, so older documents load with the stage off):

| Setting | JSON (`colorGrade.*`) | Range | Effect |
| --- | --- | --- | --- |
| `colorLut` | `colorLut.enabled`, `.path`, `.strength` | strength 0..1 | 3D `.cube` LUT applied to sRGB-encoded values, mixed over the ungraded image by `strength`. `path` is relative to the render asset root (the project's `assetRoot`) or absolute; empty applies nothing. |
| `vignette` | `vignette.enabled`, `.intensity`, `.smoothness`, `.roundness` | 0..1, 0.01..1, 0..1 | Corners keep `1 - intensity`; `smoothness` is the falloff width as a fraction of the centre-to-corner distance; roundness 1 is circular on screen, 0 follows the frame's aspect. |
| `filmGrain` | `filmGrain.enabled`, `.intensity`, `.size` | 0..0.5, 1..8 px | Uniform grain added in sRGB-encoded units over cells of `size` output pixels, re-seeded every frame by the frame serial, so `ZENITH_FRAME_DETERMINISTIC` captures repeat it exactly. |

With every effect off (or only a LUT whose file failed to load, which is
reported once on stderr) the post pass tone-maps straight into the output and
the stage records nothing: the Off fallback costs nothing and leaves images
bit-identical. Otherwise tone mapping writes an output-sized RGBA16F display
target and one fullscreen draw (`shaders/color_grade.frag`) writes the output.
The LUT is parsed on the CPU (`engine/assets/ColorLut`) and uploaded as an
RGBA16F 3D texture with linear filtering only when its path or the asset root
changes. `ColorGrading.hpp` is the CPU reference; `zenith_color_grading` covers
the parser (sizes, domains, malformed input), sampling, the vignette, grain
determinism and the settings document. `assets/color_grading/warm_cinematic.cube`
(17^3, regenerated by `tools/color_grading/`) is an example grade.

The grade is deliberately not part of `ZENITH_FORCE_HIGH_QUALITY`: it is an
artistic choice of the project, not a quality tier, so forcing it would change
every reference image rather than raise fidelity.

The scope is `gpu.color_grade` (timestamps 45/46) with the budget
`RenderPassBudgets::colorGrade` (`budgets.colorGrade`). The 2026-10-02 Release
measurement (RTX 2070 SUPER, showcase project at 1920x1080, render mode,
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
with output pixels; `gpu.post` itself is unchanged. LUT files up to the
specification's 256^3 load (about 130 MB as RGBA16F), but they are parsed and
uploaded on the render thread when the path changes, so practical grades stay
at 17^3 to 65^3.

### Post-process descriptors and FXAA

`post.frag` binds 0 HDR scene, 1 depth, 2 temporally resolved scene, 3 bloom
mip 0 and 4 the exposure buffer; the former placeholder bindings for motion,
ambient occlusion, reflections and previous motion are gone. FXAA operates on
the composited image: every FXAA tap re-evaluates the same composite as the
centre (scene plus editor grid), so grid lines are antialiased with the scene
instead of being blended against grid-free neighbours, and taps stay one
scene-image texel apart. With the grid off and full resolution the result is
unchanged. TAA frames do not run FXAA.
