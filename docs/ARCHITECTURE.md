# Architecture

Zenith is organized as a reusable engine with separate game and editor
applications. Lower-level systems provide mechanisms; applications choose the
current content, behavior, and authoring policy.

## Dependency direction

```text
main.cpp ------> DemoApplication / GameplayScene --+
                                                     |
editor/main.cpp -> EditorApplication / EditorSession +
                                                     v
                                          application lifecycle API
                                                     |
                                                     v
                                                   Engine
                         |
           +-------------+-------------+
           v             v             v
     Scene + input    Physics        Renderer
           |             |             |
           +-------> CPU assets <-------+
                                       |
                                       v
                              SDL / Vulkan / VMA
```

Important boundaries:

- Reusable engine subsystems never name a particular game project or asset.
- Scene data is expressed as components, not renderer or physics internals.
- Assets are parsed into CPU values; only the renderer owns Vulkan uploads.
- Runtime game UI is recorded through engine commands. ImGui and its Vulkan
  backend remain private renderer/editor implementation details.
- Physics uses ECS entities and Zenith domain types at its public boundary.
- Input converts SDL events into physical keyboard, mouse, and gamepad state.
  The reusable action layer combines bindings, while the demo owns action names
  and gameplay meaning.
- Demo scenes may depend on engine services, but `zenith_engine` does not link
  to `zenith_demo`.
- Project manifest parsing and project-relative scene resolution belong to the
  engine project subsystem. Startup-scene choice and gameplay composition stay
  in the application or game layer.
- Editor panels, selection, projects, commands, and document policy live in
  `src/editor/`. The editor depends on `zenith_engine`; the engine and game do
  not depend on editor code.
- `EditorSession` owns editor-only UI and persistence state. Panel visibility
  and docking layout are stored separately from runtime scene data, so changing
  the workspace cannot mutate the scene.

## Ownership and lifetime

`main` owns `Engine` and `DemoApplication`. The engine owns subsystems by value.
Initialization proceeds window, renderer, then physics. During `run`, an
`ApplicationSession` owns the active `ApplicationScene`. Shutdown unloads that
scene, stops the application, then releases physics, renderer, and window state.

Within the renderer, ownership follows Vulkan dependencies: context and device
outlive swapchain resources, passes, frame resources, and pipelines. CPU assets
live in `AssetManager`; renderer caches hold their GPU copies. Scene entities
refer to assets by generational handles rather than pointers, so storage growth
or slot recycling cannot invalidate component references silently. Scene,
editor, audio-voice, and renderer-frame leases keep reachable generations alive;
collection reclaims only unreferenced generations after in-flight consumers
retire.

Jolt bodies are owned by `PhysicsWorld`, keyed by EnTT entity. Destroying an ECS
entity does not directly call Jolt; the next physics step notices it is absent
from the component view and removes the corresponding body.

## Startup sequence

```text
main
  -> Engine::init
       -> Window::init
       -> Renderer::init
            -> Vulkan instance and SDL surface
            -> physical/logical device, queue, and VMA
            -> swapchain, depth resources, frame resources, render passes
       -> PhysicsWorld::init
  -> Engine::run(DemoApplication)
       -> Application::onStart
       -> create initial ApplicationScene
       -> ApplicationScene::load
            -> import/cache model assets
            -> instantiate ECS hierarchy
            -> add collision, bodies, controller, and camera
```

Recoverable stages return `bool` or a result structure containing an error.
`main` reports failure and still calls shutdown.

## Frame sequence

A normal rendered frame is split between variable-rate input/view work and
fixed-rate simulation:

```text
Poll SDL events and build InputState
  -> buffer pressed-key edges until a physics tick consumes them
  -> ApplicationScene::update
  -> accumulate elapsed time (clamped to 100 ms)
  -> while accumulator >= fixed step and below maxSubSteps
       ApplicationScene::fixedUpdate
       physics step
       consume buffered key presses
  -> clamp excessive backlog
  -> interpolate dynamic body transforms
  -> update scene transforms and render the HDR scene
  -> tone map, compose game UI, and present
  -> present
  -> apply deferred scene switch or quit request
```

The default physics step is 1/60 second with at most four catch-up steps. The
accumulator separates simulation stability from rendering rate. Jump input is
latched so a short press is not lost on a render frame without a physics tick.
Interpolation uses the remaining accumulator fraction between previous and
current simulated poses. A transition stages the requested scene after
presentation; only a successful load unloads the old scene and clears physics.
A failed load preserves the running scene. Asset and GPU caches remain intact
across successful transitions.

## Project and authored-scene flow

Both applications consume the engine-owned `ProjectConfig` contract. Manifest
version 3 resolves asset and scene roots relative to the project file, keeps the
startup scene contained by the scene root, and persists renderer quality and
per-pass CPU/GPU budget choices. Versions 1 and 2 remain readable; version 1
has no startup scene until it is saved in the current format.

The game executable accepts `--project <project-file>`. `DemoApplication`
selects the gameplay scene, while `GameplayScene` loads the configured authored
scene before adding demo-specific entities and behavior. Other game scenes can
load a scene-root-relative reference with `loadProjectScene`; the engine does
not assign gameplay meaning to those references.

## Asynchronous and additive scene flow

`JobSystem` provides bounded, prioritized background work with cancellation,
filtered completion delivery, deterministic single-thread pumping for tests,
and explicit shutdown. Background jobs may read and parse authored scene files,
but they do not mutate ECS registries or acquire scene-owned resources.

`SceneDocument::parse` produces an immutable validated scene description.
`SceneDocument::commit` performs the main-thread ECS and asset operations, so a
failed commit cannot publish a partially loaded additive scene.

`AdditiveSceneManager` assigns stable instance identifiers, owns an asset lease
set per committed instance, and unloads each instance deterministically. The
application supplies the desired streaming set and priorities; the manager
reprioritizes, cancels, loads, or unloads work to match it. Cross-instance
entity references pair an instance identifier with an authored stable entity
identifier and are resolved only while both remain valid.

`DerivedAssetResidencyManager` is the renderer-independent streaming policy
for derived data. CPU and GPU generations move explicitly through requested,
decoding, upload-pending, resident, eviction-pending, and retired states.
Visibility, distance, gameplay importance, and deadlines produce a stable
priority order, while separate byte and job limits bound decode and upload work
per frame. Texture mip ranges and mesh chunks are individual upload units.
Cancellation invalidates the generation immediately, so a late completion
cannot publish content after its owning scene has left the streaming set.

`PrefabSystem` is independent of editor state and can instantiate the same
versioned definition into a primary scene or any committed additive instance.
A nested instance with an explicit parent inherits that local hierarchy; a
nested instance without one is attached to the parent supplied for its owning
instance. Instance-derived stable IDs make repeated expansion deterministic.
The editor's `PrefabAuthoring` service records create, instantiate, override,
unpack, apply, and revert operations in `CommandHistory`. Applying a source
change refreshes all linked instances while reapplying their independent valid
overrides; orphaned overrides remain visible through inspection diagnostics.
Source and scene snapshots make failed operations transactional.

## Runtime game-UI flow

`Engine` owns one `GameUiContext` and exposes it to the active application
scene through `ApplicationContext`. At the start of each frame the context
captures the drawable pixel size and device-independent input snapshot. Game
code then records panels, text, texture-handle images, and buttons without
including ImGui or Vulkan headers.

The design space is 1920x1080. Uniform scaling preserves proportions at 16:9,
centers content with gutters on wider displays, and uses drawable pixels so
high-DPI windows retain stable logical sizing. Anchors, clip rectangles, draw
orders, pointer hit testing, and focused action navigation are resolved before
the renderer consumes the command list.

After the post-process pass writes the tone-mapped swapchain image, `UiPass`
translates the game commands to its private ImGui draw list and composes them
before presentation. Font bytes and texture handles cross the public boundary;
backend font atlases and Vulkan descriptors do not.

Benchmark mode supports a fixed-step full-runtime workload and a render-only
workload that freezes gameplay, animation, physics, and audio. It warms up
before recording schema-versioned CPU/GPU distributions; delayed GPU results
retain their original submission identity. The implementation roadmap and
measurement contract live in `PERFORMANCE_ROADMAP.md`.

## ECS model

`Scene` is an EnTT registry plus hierarchy operations. Every created entity
starts with identity local/world transforms and a relationship component. This
invariant lets hierarchy and transform code operate without optional branches.

Components are capabilities:

- `MeshRendererComponent` makes an entity renderable.
- `CameraComponent` makes it a camera candidate; `primary` selects the normal
  view.
- `DirectionalLightComponent` and `EnvironmentLightComponent` provide the
  primary direct, shadow, and indirect-light settings used by the renderer.
- `ColliderComponent` plus `RigidBodyComponent` and `TransformComponent` makes
  an entity discoverable by physics.
- Game-defined components can add behavior without becoming engine dependencies;
  the demo's `CharacterControllerComponent` is one example.

Imported models do not become monolithic render objects. The model root gets a
`ModelComponent`; every imported node gets its own entity and transform; nodes
with geometry get `MeshRendererComponent`. Individual nodes remain addressable
by other systems while meshes and materials remain shared assets.

## Transform contract

Zenith uses GLM column-major matrices and composes a hierarchy as
`parentWorld * local`. Positions occupy the translation column. Positive Y is
up; first-person forward maps toward negative Z after yaw is applied.

Physics decomposes world transforms into translation, rotation, and absolute
scale. Scale is baked into collision shapes. Dynamic simulation writes
translation and rotation back while preserving scale, converting the result to
parent-local space when necessary. Kinematic bodies flow the other direction:
ECS world poses are sent to Jolt each step.

## Asset flow

```text
path passed to AssetManager::loadModel
  -> canonicalized cache key
  -> cgltf parse, buffers load, validation
  -> images decoded to RGBA8 by ImageDecoder
  -> meshes reordered and simplified into LODs by MeshOptimization
  -> materials, meshes, and model nodes committed to generational asset slots
  -> typed handles returned to scene components
  -> MeshPass sees a handle on first use
  -> data staged into VMA-backed GPU resources and cached
```

This split keeps file parsing usable in CPU-only tests and prevents asset code
from inheriting GPU lifetime or synchronization concerns. Repeated loads of the
same normalized model path return the cached handle. GPU resources are uploaded
lazily only if a visible scene needs them.

Reload imports a fresh immutable generation transactionally. Failure restores
the prior slots and cache entry; success redirects future lookups while leases
keep the previous generation valid. Collection obeys CPU and decoded-audio
budgets, and renderer frame leases delay mesh, texture, descriptor, and audio
resource reclamation without device-wide or queue-wide idle waits.

Release and development paths share one contract. The composition root
initializes `ResourceLocator` from the executable path; engine and game code
then request the asset or shader root. A packaged executable finds sibling
resource directories, while a multi-config development executable may use its
parent build directory. No absolute source or build path is compiled into the
runtime.

The importer supports triangle geometry, node transforms, indexed and
non-indexed primitives, common vertex attributes, embedded/external buffers and
images, glTF metallic-roughness materials, skins, skeletal and morph-weight
animation, and opaque, masked, or blended alpha modes. Morph deltas remain CPU
asset data and active weights are selected deterministically with an eight-target
render bound. Meshopt and Draco cross an isolated decoder interface; builds
without the corresponding optional decoder return a named diagnostic.

`ProductionAssetPipeline` validates source, decoded, vertex, index, morph, and
texture limits before allocations. It validates KTX2, chooses BC7, ASTC, ETC2,
or RGBA8 from renderer capabilities, and provides the RGBA8 CPU fallback.
`DerivedAssetCache` keys deterministic output by source bytes, import settings,
tool version, and target platform under the project's `.zenith/derived`
directory. The editor inspector and `zenith_asset_build` use that same contract.

Textures are cooked offline. `zenith_asset_build cook-model-textures` derives
each external image's usage from its material slots (normal, color, or data),
builds a full mip chain, and stores BC5 (normals) or BC7 (color as sRGB, data
as linear) KTX2 in a derived cache keyed by the source bytes and cook settings.
The build cooks the shipped models into `build/derived`, and executables point
`AssetManager::setCookedTextureRoot` at `<resource root>/derived`. `Engine`
publishes the device's BC capability to the asset manager; when a format is
unsupported, or no current cooked entry exists, import decodes the RGBA8 source
and the renderer generates mips as before. Cooked textures upload every stored
level directly. BC7 images get UNORM and sRGB views; BC5 has a single linear
view, and shaders reconstruct normal Z from X and Y for every normal map.

Localization, accessibility, and platform services stay below game policy.
Stable message IDs select runtime locales and plural forms; UTF-8 shaping emits
fallback-font glyph runs. Game UI applies text/UI scaling and high-contrast
styles, captions use timed message IDs, and indicators always include a shape
and label. Versioned control profiles preserve remaps and hold/toggle choices.
Atomic user data retains a recovery copy in the platform writable directory.
Crash bundles are local JSON artifacts with no upload mechanism, while package
provenance records the exact source, dependency, toolchain, platform, and
content-manifest revisions.

## Physics flow

At the beginning of a step, `PhysicsWorld` updates world transforms and scans
for the three required physical components. Newly discovered entities become
Jolt shapes and bodies. Missing entities/components remove old bodies.
Kinematic poses are submitted, Jolt advances, and dynamic poses are copied back.

Collision layers are data-driven through `PhysicsWorldConfig`. Each has a
category bit, a mask of accepted categories, and a static/moving broad-phase
classification. Two bodies collide only when both masks accept the other.

Jolt may report contacts from worker threads. The listener collects them under
a mutex, then the step resolves body IDs to ECS entities and exposes a stable
per-step span. Read `contactEvents()` before the next step replaces it.

Changing a collider or rigid-body creation setting does not mutate an existing
Jolt body automatically. Call `refreshBody`; it removes and rebuilds the body
from current components. Forces, impulses, and velocity operations require a
body already discovered by a step or explicit refresh.

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
  -> blended surfaces -> editor outlines
Temporal resolve (TAA only, compute)
  HDR + motion/reactive + output-resolution history -> resolved scene
Motion blur (compute, when enabled)
  resolved scene (else HDR) + motion vectors -> tile max -> neighbour max
  -> reconstruction gather -> blurred scene read by everything below
Exposure and bloom (compute)
  luminance histogram -> adapted exposure buffer; bloom down/upsample chain
Post-process and tone mapping
  blurred, else resolved, else HDR scene + bloom mix + exposure + FXAA (non-TAA)
  + tone map
  -> swapchain image or editor viewport
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
lighting, the depth prepass, and GPU-driven submission. The model validates
ordering and hazards and supplies barrier and transient-memory statistics;
only its UI-boundary barriers are recorded, and the passes keep their own
synchronization.

The mesh pass maintains per-handle GPU caches. Camera buffers are per frame to
avoid CPU/GPU write races. Materials are bindless: each frame slot owns a
material-record buffer and a runtime-sized texture array whose slots are only
rewritten after that slot's previous submission retires, so no descriptor is
updated while in use. Every object record carries its material index, so draws
of one primitive batch across materials and need no per-draw binding; missing
maps use default slots. Meshes are suballocated into shared vertex/index pages
and drawn by offset. Vertices upload as quantized split streams (position,
attributes, and skin for skinned pages only); depth-style pipelines fetch only
position, skin, and, for alpha-tested shadows, UVs. Static pages bind one zero
skin influence with a dynamic stride of 0, so every mesh pipeline shares one
vertex layout.

GPU-driven submission is opt-in and uses two-phase occlusion culling. Objects
visible last frame are drawn first; a depth pyramid built from that depth
then tests every object, records its visibility for the next frame, and draws
the newly visible remainder. A compaction pass packs non-empty draws so each
pipeline and geometry page is one `vkCmdDrawIndexedIndirectCount`. Blended
surfaces and editor outlines stay on CPU submission within the same frame.

Imported primitives carry up to four LOD index ranges over shared vertices,
each with an object-space geometric error. Both submission paths pick the
coarsest level whose error, scaled by the object's largest axis scale and
projected from the nearest point of its bounding sphere at internal render
resolution, is at most `RenderSettings::lodErrorThresholdPixels` (zero keeps
full detail). The CPU path selects per render item, and shadows reuse the
camera's choice. The GPU path gives each level its own batch, chains a
primitive's levels through the batch records, and selects in the cull shader
with the same formula; every object reserves an instance slot in each of its
level batches.

With GPU-driven submission the directional shadow cascades are culled on the
GPU as well. `GpuDrivenPass` owns one output region per view
(`GpuCullLayout.hpp`): the camera's early and late phases, then one
frustum-only region per cascade. A frustum-only cull tests the sphere against
the cascade's orthographic clip volume with the same predicate the CPU cascade
cull uses, never reads the depth pyramid or the camera's visibility history,
and selects LODs from the camera's view, so cascades match the CPU path's
choice. Only cascades the cache marks dirty are culled and redrawn; their draws
use the shadow pipeline with the GPU-driven camera set, whose object binding is
the cull output, so `shadow.vert` is shared by both paths. Blended materials
never enter the GPU-driven records, matching the CPU cascade cull. The CPU path
remains the fallback whenever GPU-driven submission is off, and
`ZENITH_DISABLE_GPU_SHADOW_CULLING` selects it explicitly for comparison.

`RenderSettings::depthPrepass` (on by default) draws opaque and masked
surfaces into scene depth and a view-space normal target before shading. With
GPU-driven submission the two-phase cull runs inside the prepass, and the
shading pass redraws both phases' compacted draws without culling again. The
prepass vertex shader repeats `mesh.vert`'s position math with `precise` and
`invariant gl_Position`, so opaque shading tests `EQUAL` with depth writes off
and every opaque pixel shades once; masked materials alpha-test in both passes.
After the prepass the depth pyramid is rebuilt from the complete opaque depth
on both submission paths. With the prepass off, the main pass tests and writes
depth as before and the outputs below are unavailable.

Contract for screen-space consumers (`MeshPass::sceneGeometry` returns
`SceneGeometryViews`, valid for the frame being recorded only when the prepass
ran):

- Normals: `PostProcessPass::normalFormat` (`A2B10G10R10_UNORM`), in
  `SHADER_READ_ONLY_OPTIMAL`, output-sized with the render extent at the
  top-left. `rg` is the octahedral view-space normal mapped to `[0, 1]` (view
  space is right-handed, +Z towards the camera; back faces of double-sided
  surfaces are flipped towards the viewer), `b` is perceptual roughness clamped
  like the shading pass, and `a` is 1 where an opaque surface was written (0 on
  clear). `SceneNormalEncoding.hpp` mirrors the encoding on the CPU.
- Specular: `PostProcessPass::specularFormat` (`R8G8B8A8_SRGB`), in
  `SHADER_READ_ONLY_OPTIMAL`, same extents as the normals, written only on
  frames that trace screen-space reflections (otherwise the attachment is
  `VK_NULL_HANDLE` and `SceneGeometryViews::specular` is null). `rgb` is the
  linear specular reflectance at normal incidence (F0 =
  `mix(0.04, baseColor, metallic)`, stored through the sRGB transfer) and `a`
  is coverage. Allocated per frame slot on first use, 4 bytes per output pixel;
  `SceneSpecularEncoding.hpp` mirrors the encoding on the CPU.
- Depth pyramid: `DepthPyramid`, one RG32F image in `GENERAL` layout,
  sampled with its nearest/clamp sampler via `texelFetch`. `r` is the farthest
  (minimum, reversed Z) and `g` the closest (maximum) depth a texel covers.
  Level 0 copies scene depth over the render extent; level `L` spans
  `max(renderExtent >> L, 1)` texels in the top-left of its storage mip, and
  base pixel `p` lies in texel `min(p >> L, levelExtent - 1)`. Normalized UVs
  must be scaled by `levelExtent / storageExtent` of the sampled level.
  Readers in compute or fragment shaders are ordered before the next rebuild.

Ambient occlusion (`AmbientOcclusionPass`) is GTAO: right after the prepass
and pyramid, `gtao.comp` integrates cosine-weighted horizon visibility per
slice from pyramid level 0 and the prepass normals (view positions come from
the inverse of the jittered projection), at half or full render resolution per
`AmbientOcclusionQuality`; `gtao_denoise.comp` applies a depth-aware 5x5 blur
that covers its 4x4 noise tile. The R8 result (one image per frame slot, kept
in `GENERAL`) is camera binding 13, which `mesh.frag` samples with
`ambient_occlusion::occlusionUv` (scale in `environmentParams.w`). Its minimum
with material occlusion attenuates indirect diffuse, and Lagarde's specular
occlusion attenuates indirect specular; blended surfaces skip it. With TAA the
noise rotates per frame and the temporal resolve supplies stability. When the
prepass is off (its normals are missing) or occlusion is Off, the image is
cleared to 1 and no dispatch runs. `AmbientOcclusion.hpp` mirrors the
integration on the CPU.

Volumetric fog (`VolumetricFogPass`) is authored per scene by the primary
`VolumetricFogComponent` (density, height falloff, albedo, Henyey-Greenstein
anisotropy, noise) because the medium is content, like lights; its cost is a
project render setting, `VolumetricFogQuality` (Off, Low, Medium, High). The
froxel grid subdivides the cluster grid: each cluster tile and depth slice
splits into an integer number of froxels with the same exponential depth law,
so froxel `(x, y, z)` reads the light list of cluster
`(x / sub.x, y / sub.y, z / sub.z)` and the volume ends at the cluster grid's
`sliceEndDepth`. After light clustering and the shadow cascades, injection
writes in-scattered radiance and extinction per froxel (sun with one hardware
PCF tap of its cascade, clustered local lights unshadowed, ambient from the
primary environment's SH or the hemisphere colors), jittered and blended 1:9
with the reprojected previous frame on Medium and High. Integration then
accumulates each column front to back, slice by slice with the analytic
homogeneous-slice integral. `mesh.frag` (opaque and blended) and
`skybox.frag` composite `color * transmittance + scattering` from camera
binding 18; the skybox reads the last slice. Off records no fog work and
keeps a cleared 1x1x1 volume bound. Local-light shadows in fog are a
follow-up.

The primary directional and environment light components provide direct-light,
shadow, sky, and ground settings. Material occlusion attenuates only indirect
environment lighting, leaving direct and emissive contributions unchanged.

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

### Editor viewport

The editor requests a viewport size through `RenderRuntime`; it never receives
Vulkan images, views, samplers, or descriptor sets. `Renderer` owns a
per-frame offscreen color target plus its depth target and exposes only an
opaque `UiTextureHandle`. Viewport resize recreates those targets and the
size-dependent mesh/post-process targets without recreating the swapchain.
Descriptor replacement happens before the next ImGui frame so queued UI draw
data cannot retain a destroyed texture.

Editor camera policy, selection, and gizmo state remain in `src/editor/`.
`RenderView` is the narrow engine-facing camera override. Picking uses a
bounded CPU ray test against transformed mesh bounding spheres; this avoids a
GPU readback synchronization point. A deterministic test exercises the CPU
path with 2,000 additional renderables and reports its elapsed time without a
machine-dependent timing assertion. The renderer can replace it with an ID attachment later
without changing scene identity or editor selection state if measurement shows
that CPU picking no longer meets the interaction budget.

### Scalable lighting and environment data

Project manifest version 3 serializes every renderer quality choice plus
per-pass CPU/GPU budgets. Startup validates requested settings against device
capabilities and reports each explicit fallback. The compatible instanced draw
path remains the default; GPU-driven submission cannot be selected unless a
measured supported implementation is exposed by the renderer.

Point and spot lights are gathered by stable entity ID, capped at the configured
`maximumLocalLights` (nearest to the camera kept), and uploaded to a per-frame
storage buffer. A compute pass assigns them to a 16x9x24 cluster grid
(exponential, reversed-Z-aware depth slices over the internal render extent)
before the scene pass, and mesh.frag loops only its cluster's list. Older
settings that chose 32- or 64-light tiles load as clustered. Image-based
lighting uses the split-sum approximation: when an environment source first
appears, `EnvironmentPass` records GPU precomputation of a radiance cube, a
GGX-prefiltered specular cube, and spherical-harmonic irradiance into the frame,
caches the result by source, and retires evicted data through the frame-serial
release queue. Scenes without an environment map use an analytic sky built from
the environment colors, so lighting and the skybox stay consistent; the lowest
quality keeps hemispherical sky/ground lighting and the flat clear color.
Reflection probes use authored box volumes, blend distances, and priorities;
the dominant probe at the camera is blended over the environment. Deterministic
CPU probe preprocessing
and bake-file policy live in editor tooling. Benchmark JSON contains timestamp
scopes and configured budgets for shadow, opaque, blended, bloom/post, UI,
upload, depth prepass, depth pyramid, light clusters, and environment work,
plus CPU and estimated GPU asset memory. The editor's Graphics panel edits
every RenderSettings field, applies it through the same capability
validation, and saves it to the project file (or, without a project, to the
editor's settings directory).

## Error model

Subsystems that can provide useful diagnostics return small result values:
`SceneResult`, `AssetLoadResult`, and `PhysicsResult`. Low-level Vulkan helpers
return `bool` after reporting the failed operation. Invalid asset handles
resolve to `nullptr`; action-style physics calls return `false` when no managed
body exists. Handle outcomes where the caller can add context or recover.
