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
| Quality | project default, `ZENITH_FORCE_HIGH_QUALITY=1` + `ZENITH_FORCE_DYNAMIC_RESOLUTION=1` |

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

| Variable | Effect |
| --- | --- |
| `ZENITH_FORCE_HIGH_QUALITY` | TAA, full-resolution AO and screen-space reflections, high image-based lighting, per-pixel reflection probe blending, probe-volume indirect diffuse, clustered local lights with high-quality local shadows, depth prepass on, cascaded (high) shadows, bloom, and High volumetric fog. |
| `ZENITH_DISABLE_VOLUMETRIC_FOG` | Forces volumetric fog Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_FORCE_TAA` | Temporal anti-aliasing. |
| `ZENITH_FORCE_DYNAMIC_RESOLUTION` | Dynamic resolution with an unreachable 0.01 ms target, so the scale drops to its minimum. |
| `ZENITH_FORCE_GPU_DRIVEN` | GPU-driven submission (two-phase occlusion culling and indirect draws) when the device supports it. Shadow cascades are then culled on the GPU too. |
| `ZENITH_DISABLE_GPU_SHADOW_CULLING` | Keeps CPU shadow-cascade culling while GPU-driven submission is active (A/B baseline). |
| `ZENITH_DISABLE_SHADOW_CACHE` | Redraws every shadow cascade each frame, so shadow culling and rendering can be measured on a static scene. |
| `ZENITH_DISABLE_DEPTH_PREPASS` | Skips the depth/normal prepass even when `RenderSettings::depthPrepass` is set. |
| `ZENITH_DISABLE_RENDER_PACKET_CACHE` | Rebuilds render packets every frame (packet-cache baseline). |
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
(compare against `indirectDiffuse` set to Environment, the Off fallback). The
editor's Graphics panel shows the same table live.

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
