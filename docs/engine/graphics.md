# Graphics

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Graphics consumes scene and asset data but does not know gameplay rules:

```text
Renderer
|- VulkanContext
|- Surface
|- Swapchain (presentation images, depth image, presentation semaphores)
|- FrameResources[3] (command pool/buffer, acquire semaphore, fence)
|- RenderGraph (pass hazards, synchronization plan, transient lifetimes)
|- DebugDrawPass (debug lines over each view's output, before the UI)
`- MeshPass
   |- GPU mesh/texture/material caches
   |- shadow pass, depth/normal prepass, and main mesh pipeline
   |- DepthPyramid (min/max hierarchical depth for culling and screen-space effects)
   |- AmbientOcclusionPass (GTAO and denoise between the prepass and shading)
   |- VolumetricFogPass (froxel fog injection, reprojection, and integration)
   |- DecalClusterPass (decal records and per-cluster decal lists)
   |- ParticlePass (GPU particle emission, simulation, sorting, and drawing)
   |- TransmissionBackground (mip-chained opaque-scene copy for transmissive materials)
   `- PostProcessPass (HDR/history, motion, prepass normals, and fullscreen pipelines)
      |- ScreenSpaceReflectionPass (Hi-Z reflection trace and in-place composite)
      |- TemporalResolvePass (TAA/TAAU history)
      |- DepthOfFieldPass (thin-lens CoC, half-resolution bokeh gather, composite)
      |- MotionBlurPass (velocity tiles and reconstruction gather after TAA)
      |- BloomExposurePass (luminance histogram, exposure, bloom chain)
      |- SpatialUpscalePass (FSR 1 EASU upsample and RCAS sharpen)
      `- ColorGradePass (3D LUT, vignette and film grain after tone mapping)
```

[Graphics render views and picking](graphics-views.md) covers multi-view
frames, secondary scenes, and GPU picking; [Graphics scene passes](graphics-passes.md)
and [Graphics post-processing](graphics-post.md) cover the individual passes.

## Render flow

Each frame slot waits on its fence before reuse. The renderer then acquires a
presentation image and records one command buffer:

```text
Environment precompute (compute, only when an environment source changes)
  equirect or analytic sky -> radiance cube -> prefiltered specular + SH irradiance
GPU culling, shadow cascades (GPU-driven only, compute)
  every dirty cascade's orthographic frustum -> its own compacted indirect draws
Directional shadows
  shadow casters -> cascaded depth atlas (cached per cascade while unchanged)
  CPU path: frustum-culled render items, instanced; GPU-driven: indirect draws
GPU culling, early phase (GPU-driven only, compute)
  objects visible last frame -> compacted indirect draws
Light cluster assignment (compute, local lights enabled)
  selected point/spot lights -> 16x9x24 cluster light lists
Decal cluster assignment (compute, decals on and the scene has decals)
  nearest 1024 decal boxes -> per-cluster decal lists, applied in mesh.frag
Volumetric fog (compute, fog enabled and authored)
Particle simulation (compute, particles enabled and the scene has emitters)
  emit -> simulate, cull, and light -> bitonic sort back to front
Depth/normal prepass (default on)
  opaque + masked depth and view-space normals
  GPU-driven: early draws -> occlusion depth pyramid -> late cull -> late draws
Depth pyramid (compute)
  complete opaque depth -> min/max pyramid for screen-space consumers
  (without a prepass, GPU-driven builds it mid-shading for the late cull)
Ambient occlusion (GTAO, compute; reads the prepass normals and depth pyramid,
  writes unoccluded values without them) -> occlusion sampled by shading
Scene shading
  opaque shading (EQUAL depth after the prepass) -> HDR + motion/reactive targets
  -> skybox where opaque depth is far
  -> screen-space reflections (compute, when enabled and the prepass ran):
     Hi-Z trace of the prepass normals and depth pyramid, blended into the HDR
     target in place
  -> blended surfaces -> particles (own rendering scope, depth read-only)
  -> editor outlines
Temporal resolve (TAA only, compute)
  HDR + motion/reactive + output-resolution history -> resolved scene
Depth of field (compute, when enabled)
  resolved scene (else HDR) + motion-target depth -> thin-lens circle of
  confusion -> half-resolution bokeh gather -> full-resolution composite into
  the focused scene that exposure, bloom, and post read instead
Motion blur (compute, when enabled)
  resolved scene (else HDR) + motion vectors -> tile max -> neighbour max
  -> reconstruction gather -> blurred scene read by everything below
Exposure and bloom (compute)
  luminance histogram -> adapted exposure buffer; bloom down/upsample chain
Post-process and tone mapping
  final scene (blurred, else focused, else resolved, else HDR) + editor grid,
  FXAA (non-TAA) over that composite,
  exposure + bloom mix + tone map
  -> swapchain image or editor viewport, or the colour-grade display target
Colour grade (only when a LUT, vignette or film grain is enabled)
  display target -> 3D LUT (sRGB-encoded) -> vignette -> film grain
  -> swapchain image or editor viewport
Debug lines (per view that has lines; see DebugDrawPass below)
Game or editor UI
  -> swapchain image
Transition to PRESENT -> submit -> present
```

The ambient-occlusion and reflection techniques, and where they sit relative
to shading and the temporal resolve, belong to their own passes; the list
shows their position at the time of writing. Each bracketed GPU pass has a
timestamp pair (`FrameResources::collectTimings`, `FrameTimestamps.hpp`); the
decoded durations reach `RenderStats`, benchmark `gpu.*` metrics, and the
editor's Graphics panel. `Renderer::updateRenderGraphPlan` mirrors this list as
a declarative render-graph model whose shape (`RenderGraphShape`) follows the
settings that add or remove passes: anti-aliasing, AO and reflection quality,
bloom, exposure mode, shadows, environment quality and background, local
lighting, the depth prepass, depth of field, and GPU-driven submission. The
shared passes appear once and the per-view passes (clusters onward) once per
recorded render view (see [Render views](graphics-views.md#render-views)).
Every choice the mesh pass makes while recording (prepass, GPU-driven
submission, GPU shadow culling, effective AO quality, screen-space reflections,
per-pixel probes) comes from one `MeshPassFramePlan` (`MeshPassFramePlan.*`)
per view that `MeshPass::planFrame` computes once per frame; `MeshPass::record`
consumes it and the Renderer copies it into `RenderGraphShape`, so the model
never re-derives a pass decision. The model validates ordering and hazards and
supplies barrier and transient-memory statistics; only its UI-boundary
barriers are recorded, and the passes keep their own synchronization.

The renderer uses Vulkan dynamic rendering and synchronization2 barriers for
explicit layout/access transitions. A declarative render graph validates pass
ordering, hazards, queue ownership, and compatible transient lifetimes; its
compiled UI-boundary barriers are used directly during recording. Renderer
uploads use a three-slot staging ring, a dedicated transfer queue, and timeline
retirement when available. A full ring reports backpressure and defers the
asset instead of waiting; waits are scoped to the consuming pipeline stages.
Startup prewarms the fixed pipeline set through a device-compatible Vulkan
pipeline cache persisted with an atomic user-data write. Benchmark telemetry
separates pipeline creation from first use. The swapchain owns
presentation-dependent resources. Resize waits for a usable surface, idles the
device, recreates the swapchain/depth resources, and resizes dependent render
targets, invalidating temporal history.

Temporal reconstruction owns jitter, motion/reactive data, camera-cut and
resize invalidation, and variance-clipped history. Jitter spans one internal
pixel; motion vectors are unjittered and skinned motion uses the previous
frame's joint palettes. History lives at output resolution and is shared by
consecutive frames regardless of the frame slot, so dynamic-resolution changes
keep it: the resolve upsamples each frame from its internal extent. Dynamic
resolution is driven by smoothed retired GPU time and adjusts slowly in
quantized steps; native resolution remains the deterministic default.

Mesh-shader and hardware-ray-tracing capability bits are informational only.
Neither path is exposed as a quality setting until the persistent GPU scene is
stable and repeated measurements justify a separate implementation and memory
budget; conventional rasterization therefore remains the explicit path.

### Settings, capabilities, and budgets

The project manifest serializes every renderer quality choice plus per-pass
CPU/GPU budgets (see [Projects](projects.md)). Startup validates requested
settings against device capabilities and reports each explicit fallback
(`ValidatedRenderSettings::fallbacks`). The compatible CPU instanced draw path
is the default; GPU-driven submission is opt-in through
`RenderSettings::preferGpuDrivenSubmission` (or `ZENITH_FORCE_GPU_DRIVEN`) on
devices that support it, a decision recorded in
[Renderer validation](../RENDERER_VALIDATION.md#gpu-driven-submission-default-milestone-25).
Benchmark JSON contains timestamp scopes and configured budgets for shadow,
opaque, blended, bloom/post, UI, upload, depth prepass, depth pyramid, light
clusters, and environment work, plus CPU and estimated GPU asset memory. The
editor's Graphics panel edits every `RenderSettings` field, applies it through
the same capability validation, and saves it to the project file (or, without
a project, to the editor's settings directory).

## `src/engine/graphics/Renderer.*`

Coordinates initialization, per-frame work, resize recovery, and shutdown. A
frame waits for its reusable slot, acquires a swapchain image, records mesh and
post-processing commands, submits them, and presents. Out-of-date/suboptimal
surfaces trigger recreation; zero-sized surfaces are skipped until usable. It
owns the offscreen targets of every render view and turns the `RenderRuntime`
view requests into the frame's ordered `MeshPassViewRequest` list.

## `src/engine/graphics/Rendering.*`

Plain render-state types shared by applications and the renderer:
`RenderSettings`, capabilities, statistics, and `RenderRuntime`, which holds the
render-view registry (generational `RenderViewId`s, per-view size, camera
override, grid, outlines, settings override, published texture and
`RenderViewStats`). `composeViewSettings` defines which settings a view may
override. `tests/engine/RenderViewTests.cpp` covers the registry.
It also owns the frame's debug-draw lists (all views and per view) and each
view's projected debug labels.

It also holds the secondary render scenes (`createScene`, `setViewScene`,
`setSceneContent`; see [Secondary scenes](graphics-views.md#secondary-scenes)), which
`MeshPass` draws from per-scene packet state (`passes/MeshPassScenes.cpp`) with
preview lighting. `tests/engine/TwoSceneSmokeTests.cpp` renders one beside the
application's scene and picks in it.

Cost: a bound scene costs nothing unless a view of it was requested this
frame (`requestViewSize`): the renderer then leases its assets for the frame
in flight and `MeshPass` builds and records it. `RenderStats` reports how many
secondary scenes a submission drew, the CPU time they took
(`cpuSecondarySceneMilliseconds`), and their packet rebuilds. Measured with
`zenith_secondary_scene_benchmark` (Release, lavapipe, 500 cubes per scene),
the renderer's CPU time is about 0.55 ms + 0.26 ms per drawn scene, ~0.23 ms
of which is recording its view; a bound but hidden scene adds nothing
measurable.

`RenderIds.hpp` holds `RenderViewId`, `RenderSceneId`, and `RenderObjectId` so
the picking types can name them.

## `src/engine/graphics/DebugDraw.*`

Renderer-independent immediate-mode recording of debug lines, wire shapes
(arrow, AABB and oriented box, sphere, circle, frustum, cone, capsule) and
text labels, plus `frustumCorners` and the label projection
(`projectDebugPoint`, `projectDebugLabels`). No graphics types, so physics and
gameplay can record into it.

Lifetimes and channels: `DebugDraw`'s own shape functions record one-frame
primitives into the default channel, unchanged from before. A
`DebugDrawRecorder` records into a named channel (`channel("gen.rooms")`),
for a duration (`forSeconds(2.0f)`, drawn in at least one frame), or until
cleared (`persistent()`), and they combine:
`debugDraw.channel("gen.rooms").persistent().aabb(...)`. Channels can be
hidden (`setChannelVisible`, kept but not drawn; one-frame shapes recorded
into a hidden channel are dropped) and cleared as a group (`clearChannel`);
`channels()` lists the named ones with whether they are active.
`endFrame(elapsedSeconds)` ends a frame: the renderer calls it after every
frame with the wall-clock time since the previous one
(`RendererOptions::fixedFrameSeconds` when set), so durations are real time
and also elapse while the game is paused and in the editor's Edit mode.
`lineVertices`, `lineCount`, and `labels` return what is visible now, composed
from every lifetime of every visible channel. `tests/engine/DebugDrawTests.cpp`
covers it.

## `src/engine/graphics/passes/DebugDrawPass.*`

Draws `DebugDraw` lines (`shaders/debug_line.*`) as a LINE_LIST into every
recorded view's post-processed output, depth testing in the fragment shader
against the view's scene depth (which may be at render resolution). Owns a
host-visible vertex buffer per frame in flight, grown on demand.

`DebugDraw` is plain data with no graphics types, so any subsystem it is
passed to can record. Colours are display-referred RGBA; each primitive is
depth tested or drawn always on top. `RenderRuntime::debugDraw()` holds the
primitives every view draws and `viewDebugDraw(view)` those of one view; there
is no global instance. Primitives recorded during update and UI building are
drawn by the next `Renderer::drawFrame`, which ends every list's frame
(`DebugDraw::endFrame`) when it returns, also when the frame is skipped. The
editor's viewport Overlays menu lists the channels.

The pass draws in every recorded view after the post chain (tone mapping,
FSR, colour grade) and before the UI, into the view's display-referred output:

- Colours are exact (exposure, bloom, and the grade never touch them), and
  lines use the unjittered projection and never enter the TAA history, so
  they neither shimmer nor smear.
- Depth-tested lines compare their forward distance with the view's reversed-Z
  scene depth, fetched in the fragment shader at the texel under each output
  pixel. The depth buffer can be at a lower render resolution than the output
  (dynamic resolution, TAAU, FSR 1), so a depth attachment test would not
  line up. A 0.4% relative bias keeps lines lying on surfaces visible.
- Lines are 1 pixel wide and not anti-aliased.

The render graph declares a `debug draw` pass per view (reading scene depth,
read-writing the output) when the view has lines, so the barrier that hands an
offscreen view to the UI comes from it. Labels are not rasterized by the
engine: after recording, the renderer projects each view's labels with the
camera it rendered (`projectDebugLabels`) and publishes them through
`viewDebugLabels(view)` in view pixels for the editor or game UI to draw;
labels behind the camera or outside the view are dropped. The editor draws
them over each viewport; `zenith --project` draws the default view's as game
UI text (`src/game/application/DebugLabelOverlay`, through
`GameUiContext::pixelText`), a frame after they were recorded.
`tests/engine/DebugDrawTests.cpp` covers tessellation, projection, and the
Jolt adapter on the CPU; `tests/engine/DebugDrawSmokeTests.cpp` draws in three
views under strict validation and renders the `debug_draw` reference set.

## `src/engine/graphics/RenderPicking.*`, `passes/IdBufferPass.*`, and `resources/IdReadback.*`

GPU picking (see [Picking](graphics-views.md#picking)). `RenderPicking`
holds the CPU contract: `PickRect`, `PickTicket`, `PickResult` with its id and
depth planes and object table, `PickRequestBook` (the ticket lifecycle
`RenderRuntime::requestPick` and `pickResult` expose), and depth-to-world
reconstruction. `IdBufferPass` owns the id-buffer pipeline
(`shaders/id_buffer.*`) and targets and draws one pick region; `MeshPass`
feeds it the view's visible items only when a request is pending.
`IdReadback` copies a region into a host buffer the `Renderer` reads after the
frame's fence signals. `tests/engine/RenderPickingTests.cpp` and the Vulkan
`tests/engine/PickingSmokeTests.cpp` cover them.

## `src/engine/graphics/RenderViewPlanning.*`

Pure multi-view decisions: the recording order that puts the active view first
(it leads the frame's shared work) and the remap of the leading view's
local-shadow tiles onto another view's light selection. See
[Recording order](graphics-views.md#recording-order).

## `src/engine/graphics/vulkan/VulkanContext.*`

Owns the Vulkan instance, optional validation messenger, selected physical
device, logical device, graphics/presentation queue, VMA allocator, and Vulkan
pipeline cache. Device selection checks the required Vulkan features and
presentation support. Compatible cache bytes are loaded from the platform user
data directory and atomically persisted before device destruction.

## `src/engine/graphics/surface/Surface.*`

Owns the SDL-created `VkSurfaceKHR` and queries capabilities, formats,
presentation modes, queue presentation support, and format support.

## `src/engine/graphics/swapchain/Swapchain.*`

Chooses extent, BGRA8 sRGB output, a supported depth format, and presentation
mode. It owns the swapchain, image views, per-image render-complete semaphores,
and a VMA-backed depth image/view. FIFO is the portable fallback. With
`SwapchainOptions::transferSource` its images also allow copies for frame
capture (`resources/ImageReadback.*`, written to PNG by `diagnostics/`).

## `src/engine/graphics/resources/FrameResources.*`

Owns one frame slot's command pool, command buffer, image-acquired semaphore,
completion fence, and timestamp queries. Three instances prevent reuse while
the GPU is working. Timeline waits are attached only to stages that consume
uploaded buffers or textures. `collectTimings` decodes each timestamp pair into
`GpuFrameTimings`.

## `src/engine/graphics/resources/FrameTimestamps.hpp`

Names the timestamp slots of passes a frame may skip (depth prepass, depth
pyramid, light clusters, environment precompute, skybox, shadow-cascade
culling).
`OptionalPassTimestamps` writes each slot at most once and closes skipped pairs
back to back, so every query becomes available and skipped passes read zero.

## `src/engine/graphics/resources/UploadContext.*`

Owns a bounded three-slot staging ring. Timeline-capable queues are polled for
retirement without blocking; if every slot is still in flight, staging reports
backpressure so the caller can defer work. Fence retirement is the correctness
fallback on devices without timeline semaphores. Timestamp and byte counters
remain attributable to retired uploads.

## `src/engine/graphics/resources/BindlessTextureTable.*`

CPU bookkeeping for the bindless texture array: stable slot assignment, reuse,
and a per-frame dirty mask so each frame slot's descriptor set is rewritten only
for slots that changed. Released slots fall back to a default view. `replace`
points a live slot at a new view (a streamed texture's wider mip range) without
changing its index, so material records stay valid.

## `src/engine/graphics/resources/MipChainBlit.*`

Records the linear-blit mip chain shared by material RGBA8 uploads and the
environment pass. The layout contract is explicit: every level starts in
`TRANSFER_DST_OPTIMAL` with level 0 written and ends `SHADER_READ_ONLY_OPTIMAL`,
visible to caller-named consumer stages. `planMipChainBlits` is the CPU-testable
extent plan the recorder follows.

## `src/engine/graphics/resources/GeometryArena.*`

Suballocates meshes into shared vertex/index pages with VMA virtual blocks,
opening a new page (sized to fit oversized meshes) when none has room. Pages
have a static or skinned layout so only skinned pages carry a skin stream. It
owns no Vulkan buffers; the mesh pass creates one buffer per stream plus an
index buffer for each reported page.

## `src/engine/graphics/graph/RenderGraph.*`

Compiles imported and transient resource declarations into a stable pass order,
synchronization2 hazard plan, queue transfers, and compatible lifetime aliases.
Its statistics report requested, committed, aliased, and peak transient bytes;
debug builds compare the result with conservative synchronization.

## `src/engine/graphics/shader/ShaderModule.*`

A short-lived RAII wrapper that reads SPIR-V and creates a `VkShaderModule` for
pipeline creation. Its destructor releases the module.

## `src/engine/graphics/pipeline/GraphicsPipeline.*`

Builds and owns the main mesh, depth-normal prepass, or depth-only shadow
pipeline and layout using Vulkan dynamic rendering. After a prepass, opaque
mesh pipelines test `EQUAL` without writing depth.

## Folder contents

`src/engine/graphics/`: Renderer and Vulkan implementation; nested folders
separate passes, resources, pipelines, surfaces, swapchains, shaders, and
Vulkan setup.
