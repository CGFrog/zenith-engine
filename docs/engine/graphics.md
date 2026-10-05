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

`RenderIds.hpp` holds `RenderViewId` and `RenderObjectId` so the picking types
can name them.

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
`docs/ARCHITECTURE.md` (Debug drawing) explains the placement.

## `src/engine/graphics/RenderPicking.*`, `passes/IdBufferPass.*`, and `resources/IdReadback.*`

GPU picking (see Picking in [Architecture](../ARCHITECTURE.md)). `RenderPicking`
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
local-shadow tiles onto another view's light selection.

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
