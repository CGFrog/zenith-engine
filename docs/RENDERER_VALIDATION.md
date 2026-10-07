# Renderer validation

Renderer changes stay measurable and capability-gated. This page holds the
current gates and tools: the performance A/B gate, image stability, frame
capture and reference images, the Vulkan smokes, strict validation, the
diagnostic environment variables, GPU pass metrics, and the silent-fallback
audit. Feature contracts (settings, fallbacks, GPU scopes, tests) live in the
subsystem pages ([Graphics](engine/graphics.md),
[Graphics scene passes](engine/graphics-passes.md),
[Graphics post-processing](engine/graphics-post.md)); dated measurement logs
are archived in [Renderer measurements](history/renderer-measurements.md).

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

Earlier results of this gate are in
[Renderer measurements](history/renderer-measurements.md#render-packet-cache-2026-09-27).

GPU-driven (indirect) submission is available only when descriptor indexing,
multi-draw indirect, and indirect draw count are all supported. The compatible
instanced path stays the default unless a same-device benchmark improves by at
least 10%; see the decision below.

### GPU-driven submission default (Milestone 25)

Decision: CPU (instanced) submission stays the default and there is no
automatic threshold, because no measured draw or instance count produced a
GPU-frame win. GPU-driven submission stays opt-in through
`RenderSettings::preferGpuDrivenSubmission` or `ZENITH_FORCE_GPU_DRIVEN`, and
remains covered by the validation matrix. Revisit the decision with a
vertex- or fill-bound occluded scene (many distinct meshes or materials,
expensive shading) where CPU draw counts grow with visible batches.

The measurement behind the decision is in
[Renderer measurements](history/renderer-measurements.md#gpu-driven-submission-default-2026-10-02).

## Image stability

`engine.RenderRegression` compares deterministic color-space, cascade-shadow,
edge-antialiasing, and final game-UI composition captures with sealed hashes.
The UI capture covers the post-tone-map ordering and expected output color
space. On failure it writes the candidate PPM images to the system temporary
directory for inspection.

The public anti-aliasing choices are **Off**, **FXAA**, and **TAA**
(`AntialiasingMode`). TAA is the temporal upscaling resolve
(`TemporalResolvePass`): it reprojects history with per-pixel motion vectors,
rejects disoccluded and reactive pixels, and resolves the jittered internal
resolution to the output. `engine.TemporalQuality` covers jitter, history
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
assets were still uploading on the captured frame. A capture window is fixed
size and never takes focus, so another program cannot maximize, snap, or send
keys to it (a maximized window once produced a 2560x1377 capture under a busy
desktop); if its drawable size still changes during the run, the run fails
naming both sizes, and the reference driver prints the capture and reference
sizes on any dimension mismatch.

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

- `showcase/` renders the project's own settings, so every view also carries
  probe-volume indirect diffuse, the mild default depth of field, Low motion
  blur, and the LUT, vignette and film-grain grade. It covers converged and
  adapting auto exposure, TAAU ghosting during a lateral pan, alpha-tested
  foliage edges through the depth prepass, half-resolution GTAO contact
  occlusion, screen-space reflections, bloom, the KHR material gallery
  (clearcoat, sheen, transmission, specular, ior), clustered decals (floor,
  wall with a normal map, and a box straddling a wall corner that must not
  leak), GPU particles (additive embers and lit smoke), depth of field on a near
  sphere, camera motion blur (a 180 degree/s yaw) and object motion blur (the
  fast tentacle under a static camera), the colour grade over a wide view, and
  FSR 1 at the Performance preset. Views whose feature an image-wide default
  would mask switch that default off in their `environment`
  (`ZENITH_DISABLE_DEPTH_OF_FIELD` for the foliage, GTAO, SSR, material and
  object-blur views; `ZENITH_DISABLE_MOTION_BLUR` for TAAU ghosting). Film
  grain is seeded by the frame serial, so it repeats exactly and needs no extra
  tolerance; every view uses the default limits.
- `irradiance_room/` covers volumetric fog light shafts through the room's
  window (dense uniform fog, ambient-lit by the baked irradiance volume) and
  the volume's bounce lighting and color bleed.
- `materials/` renders `projects/material_assets.zenith-project`: three cubes,
  one with its own material, one whose `MaterialOverrideComponent` names the
  `.zmat` material `assets/materials/capture_tiles.zmat` (base color and
  normal maps), and one with that material under override factors.
- `debug_draw/` renders `zenith_debug_draw_smoke_tests` instead of the game
  (no project, fixed camera): every `DebugDraw` primitive over three cubes,
  depth-tested lines partly hidden by and intersecting them, always-on-top
  lines across them, and an alpha-blended circle, natively and with FSR 1 at
  its Performance preset (lines against lower-resolution scene depth).
- `scene_gizmos/` renders `zenith_scene_gizmo_smoke_tests` (no project,
  fixed lens on the default view): every editor viewport scene gizmo
  (`editor/viewport/SceneGizmoDraw`) with its category set to All over a small
  scene with lights, two cameras, colliders, a reflection probe, a decal, an
  irradiance volume, and an active plus a selected cube. Running it with
  `ZENITH_SCENE_GIZMOS=off` records no gizmos, which must fail the comparison.

Each view
names a capture frame, a camera pose and optional motion (both optional for a
capture executable with its own fixed camera), optional extra environment
variables, and optional per-view tolerances. A manifest without `project`
launches its executable without `--project`. The checked-in PNGs
beside it are the references.

`zenith_reference_capture` renders every view with `zenith` (or the set's
own executable, chosen in `tests/capture/CMakeLists.txt`), a clean
environment (benchmark, smoke, and every `ZENITH_FORCE_*`, `ZENITH_DISABLE_*`
`ZENITH_DEPTH_PYRAMID_PER_LEVEL` and device-fallback switch removed before the view's own
`environment` entries are applied), the
manifest's window size, and `ZENITH_FRAME_DETERMINISTIC=1`, then compares each
capture with its reference in `engine/diagnostics/ImageComparison`. Differences
are per RGB channel in 8-bit units: a pixel differs when any channel exceeds
`channelTolerance`, and a view fails when the RMSE, the largest channel
difference, or the fraction of differing pixels exceeds its limit. A failure
writes `<view>.diff.png` (amplified absolute difference) beside the capture in
`build/captures/<set>/`. Consecutive runs on one machine differ by at most a
few 8-bit steps in a handful of pixels; the default limits (3 per channel, RMSE
1.5, 0.1% of pixels) leave room for that and nothing larger.

Each manifest view is its own CTest, `zenith_<set>_capture_<view>` (labels
`vulkan;capture`), registered from `captures.json` at configure time, so views
capture in parallel and a failure names its view; editing a manifest re-runs
configure. Each returns 77, reported as skipped, when the machine has no
hardware Vulkan device. CI runs them in the Vulkan-labelled step only.

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
and Stop, then opens a second Viewport beside the first with another camera and
Wireframe shading, checks that both render views rendered with their own
shading while the project settings stay Rendered (the first with every scene
overlay set to All, checking it projected name labels), and closes it again,
checking its view was destroyed. It is labeled `vulkan;smoke` so headless CI can exclude it while a
Vulkan-capable workstation runs it explicitly.

The `zenith_debug_draw_vulkan_smoke` CTest (`vulkan;smoke`, strict
validation) records the debug-draw primitive set into the default view and two
created views every frame, one of them upscaled with FSR 1, and checks that the
renderer clears the lists after each frame and that each view publishes its
projected labels (shared and own, none behind the camera).
`zenith_scene_gizmo_vulkan_smoke` (`vulkan;smoke`, strict validation) records
the editor's scene gizmos into the default view's own list every frame and
checks the list is cleared and the active object's name label is projected.

`zenith_picking_vulkan_smoke` (`tests/engine/PickingSmokeTests.cpp`) picks two
cubes through a created view and the default view: a 3x3 pick at the centre
cube's projected centre must name it and reconstruct its front face from the
depth plane, a pick on empty space must return no object, a pick larger than
the view must clamp and name both cubes, and `RenderStats::pickPasses` must be
zero on every frame without a request. Picks whose view is resized or
destroyed first must resolve to Cancelled. `zenith_validation_picking` and
`zenith_validation_picking_gpu_driven` run it under strict validation on both
submission paths.

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
| Quality | project default, `ZENITH_FORCE_HIGH_QUALITY=1` + `ZENITH_FORCE_DYNAMIC_RESOLUTION=1`, the same plus `ZENITH_FORCE_UPSCALER=fsr1` + `ZENITH_FORCE_UPSCALER_QUALITY=performance` (`fsr1_dynamic`), `ZENITH_FORCE_HIGH_QUALITY=1` with every device-fallback switch below and FSR 1 requested (`device_fallbacks`) |

for 120 frames each; each content and submission pair again with
`ZENITH_FORCE_HIGH_QUALITY=1` and each view shading mode other than Rendered
(`ZENITH_FORCE_VIEW_SHADING=material_preview|solid|wireframe`, tests named
`..._shading_<mode>`); Wireframe with `ZENITH_DISABLE_WIREFRAME=1`
(`zenith_validation_demo_cpu_shading_wireframe_fallback`, the Solid
fallback); and the scripted editor smoke (`zenith_validation_editor`), all
with `ZENITH_VALIDATION_STRICT=1`:

```powershell
ctest --test-dir build -C Debug --output-on-failure -L validation
```

The matrix must report zero validation and synchronization messages; the
issues the first strict run found are recorded in
[Renderer measurements](history/renderer-measurements.md#first-strict-vulkan-validation-run).

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
| `ZENITH_FORCE_HIGH_QUALITY` | TAA, half-resolution AO (the recommended quality; see [Ground-truth ambient occlusion](engine/graphics-passes.md#ground-truth-ambient-occlusion)), full-resolution screen-space reflections, high image-based lighting, per-pixel reflection probe blending, probe-volume indirect diffuse, clustered local lights with high-quality local shadows, depth prepass on, cascaded (high) shadows, bloom, High volumetric fog, High depth of field, High motion blur, clustered decals, and High GPU particles. Colour grading and the upscaler are project choices and are not forced. |
| `ZENITH_DISABLE_VOLUMETRIC_FOG` | Forces volumetric fog Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_DISABLE_DEPTH_OF_FIELD` | Forces depth of field Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_DISABLE_MOTION_BLUR` | Forces motion blur Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_DISABLE_PARTICLES` | Forces GPU particles Off (applied after `ZENITH_FORCE_HIGH_QUALITY`). |
| `ZENITH_FORCE_TAA` | Temporal anti-aliasing. |
| `ZENITH_FORCE_DYNAMIC_RESOLUTION` | Dynamic resolution with an unreachable 0.01 ms target, so the scale drops to its minimum. |
| `ZENITH_FORCE_UPSCALER=off\|taau\|fsr1` | Sets `RenderSettings::upscaler.mode` (`native` is an alias of `off`); `taau` also forces TAA. Unknown values are ignored. Forced FSR 1 on a device without support renders at native resolution. |
| `ZENITH_FORCE_UPSCALER_QUALITY=quality\|balanced\|performance` | Sets the upscaler preset (1.5x, 1.7x, 2x per axis). |
| `ZENITH_FORCE_VIEW_SHADING=rendered\|material_preview\|solid\|wireframe` | Sets `RenderSettings::viewShading` (see [View shading modes](engine/graphics-passes.md#view-shading-modes)). Unknown values are ignored; forced Wireframe still draws Solid without the wireframe capability. |
| `ZENITH_DISABLE_WIREFRAME` | Device fallback: leaves `fillModeNonSolid` disabled on devices that support it, so Wireframe view shading falls back to Solid. |
| `ZENITH_FORCE_GPU_DRIVEN` | GPU-driven submission (two-phase occlusion culling and indirect draws) when the device supports it. Shadow cascades are then culled on the GPU too. |
| `ZENITH_DISABLE_GPU_SHADOW_CULLING` | Keeps CPU shadow-cascade culling while GPU-driven submission is active (A/B baseline). |
| `ZENITH_DISABLE_SHADOW_CACHE` | Redraws every shadow cascade each frame, so shadow culling and rendering can be measured on a static scene. |
| `ZENITH_DISABLE_DEPTH_PREPASS` | Skips the depth/normal prepass even when `RenderSettings::depthPrepass` is set. |
| `ZENITH_DISABLE_RENDER_PACKET_CACHE` | Rebuilds render packets every frame (packet-cache baseline). |
| `ZENITH_DISABLE_BLOCK_COMPRESSION` | Device fallback: leaves BC sampling disabled on devices that support it, so textures upload as RGBA8 (the path of devices without `textureCompressionBC`). Logged once. |
| `ZENITH_FORCE_UPDATE_AFTER_BIND` | Device fallback: builds the bindless material table with update-after-bind descriptors, the path taken by devices whose ordinary sampled-image limits cannot hold the table. Logged once; ignored without the feature. |
| `ZENITH_DISABLE_SPATIAL_UPSCALING` | Device fallback: reports FSR 1 as unsupported, so a requested FSR 1 falls back to TAA upsampling (or native resolution). |
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
`gpu.light_clusters`, `gpu.reflection_probe_clusters`, `gpu.decal_clusters`, `gpu.depth_prepass` (which includes the GPU-driven late
cull), `gpu.depth_pyramid`, `gpu.ambient_occlusion`, and shading. `gpu.blended` includes `gpu.skybox`;
`gpu.reflections` runs between them inside the scene pass and is subtracted from `gpu.blended`.
`gpu.environment` is precompute plus skybox. `gpu.volumetric_fog` (fog
injection, reprojection, and integration) runs inside `gpu.opaque` after
`gpu.light_clusters` and reports zero when fog is Off or the scene has no
`VolumetricFogComponent`; `render.volumetric_fog_bytes` reports its volumes.
`gpu.particle_simulate` (particle emission, simulation, lighting, and sorting)
also runs inside `gpu.opaque`, after the fog; `gpu.particle_draw` (the billboards)
runs inside `gpu.blended`. Both report zero when particles are Off or the scene
has no `ParticleEmitterComponent`; `render.particle_emitters`,
`render.particle_capacity` (pool slots the emitters own), and
`render.particle_bytes` describe the pool.
`gpu.irradiance_volume` is the
irradiance-volume probe upload, which records work only on frames whose bound
volume changed and also falls inside `gpu.shadow`; sampling the volume is part
of opaque and blended shading, so its per-pixel cost shows up in `gpu.opaque`
(compare against `indirectDiffuse` set to Environment, the Off fallback).
`gpu.depth_of_field` and then `gpu.motion_blur` run after
`gpu.temporal_resolve` and before `gpu.exposure`, and report zero when Off.
`gpu.fsr_easu` and `gpu.fsr_rcas` follow `gpu.post` when FSR 1 is selected, and
`gpu.color_grade` is the colour-grade draw after them (zero when every grade
effect is off; see [Colour grading](engine/graphics-post.md#colour-grading)). `gpu.decal_clusters` runs beside the light
clusters, `gpu.particle_simulate` after the volumetric fog, and
`gpu.particle_draw` inside the `gpu.blended` span. `gpu.transmission_background`
is the opaque-scene copy for transmissive materials, inside the `gpu.blended`
span and subtracted from it (see
[KHR material extensions](engine/graphics-passes.md#khr-material-extensions)). The editor's Graphics
panel shows the same table live.

`gpu_visibility_mismatches` counts retired GPU-driven frames whose culling kept
more objects than the CPU frustum reference. The GPU sphere test uses the
reference's exact x/y planes and tighter depth planes, so any nonzero value is a
culling defect.

Per-pass costs measured on the reference hardware are in
[Renderer measurements](history/renderer-measurements.md#per-pass-gpu-cost-on-the-reference-hardware-2026-10-02).

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
| Scene decals over `maximumDecals` (1024; nearest kept) | `RenderStats::droppedDecals`, `decals.dropped` |
| Cluster decal assignments over the per-cluster capacity (31; the decals applied last are dropped) | `RenderStats::discardedDecalAssignments`, `decals.discarded_assignments` |
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

Per-frame capacities a large scene can exceed (render items over
`maximumObjectsPerFrame`, primitives whose material finds no record slot,
skinned items over the per-frame joint-matrix capacity, and material overrides
over `maximumMaterialOverrides`) degrade instead of failing the frame: each
overflow is logged once per episode and counted in `RenderStats`
(`droppedRenderItems`, `droppedSkinnedRenderItems`; see
[Graphics scene passes](engine/graphics-passes.md#srcenginegraphicspassesmeshpass)).
The GPU-driven LOD-instance and draw-group capacities still fail the frame and
log an error each frame they are exceeded. An environment map texture without
usable pixels still falls back to the analytic sky without a message.

## Feature contracts

Each feature's settings, fallback, GPU scope, budget, and tests are documented
with its pass:

- [Upscaling](engine/graphics-post.md#upscaling), [Motion blur](engine/graphics-post.md#motion-blur),
  [Depth of field](engine/graphics-post.md#depth-of-field),
  [Colour grading](engine/graphics-post.md#colour-grading), and
  [Post-process descriptors and FXAA](engine/graphics-post.md#post-process-descriptors-and-fxaa).
- [Ground-truth ambient occlusion](engine/graphics-passes.md#ground-truth-ambient-occlusion),
  [Single-pass depth pyramid](engine/graphics-passes.md#single-pass-depth-pyramid),
  [GPU-culled shadow cascades](engine/graphics-passes.md#gpu-culled-shadow-cascades),
  [GPU particles](engine/graphics-passes.md#gpu-particles),
  [Reflection probe blending](engine/graphics-passes.md#reflection-probe-blending),
  [Clustered decals](engine/graphics-passes.md#clustered-decals),
  [KHR material extensions](engine/graphics-passes.md#khr-material-extensions),
  [Local-light shadows](engine/graphics-passes.md#local-light-shadows), and
  [View shading modes](engine/graphics-passes.md#view-shading-modes).
