# Repository guide

This guide is a map of Zenith's checked-in files. It explains what each element
owns, what depends on it, and where a change belongs. For the runtime sequence
and subsystem contracts, see [Architecture](ARCHITECTURE.md). For commands and
common changes, see [Development](DEVELOPMENT.md). The remaining optimization
objectives and their measurement gates are in
[Performance roadmap](PERFORMANCE_ROADMAP.md).

## Top level

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Declares the C++20 project, pins third-party dependencies, builds the engine, executable, shaders, assets, and tests, and supplies runtime directory definitions. |
| `README.md` | Quick-start overview, controls, current feature summary, and links into these detailed guides. |
| `AGENTS.md` | Root contributor policy and navigation. Scoped guides under `src/engine/`, `src/editor/`, `src/game/`, and `tests/` add local ownership rules. |
| `.clang-format` | Formatting rules for C++ headers and sources. |
| `.gitignore` | Excludes generated and local files such as the build tree. |
| `.vscode/` | Optional editor configuration. It is not required by the build. |
| `assets/` | Source runtime content copied into the build tree. |
| `shaders/` | GLSL source compiled to SPIR-V during the build. |
| `src/` | Application composition and reusable engine implementation. |
| `tests/` | CPU-side subsystem and integration tests registered with CTest. |
| `build/` | Generated configuration, dependencies, binaries, copied assets, and compiled shaders. Do not edit generated contents. |

## Workspace layout

```text
src/
|- engine/                 reusable runtime library
|  |- application/         lifecycle contracts
|  |- assets/              CPU asset import and storage
|  |- audio/               decoding, streaming, and mixing
|  |- graphics/            rendering and Vulkan implementation
|  |- input/ jobs/ physics/ resources/ scene/ ui/ window/
|  `- project/             shared project manifests and scene paths
|- editor/                 authoring application
|  |- application/ document/ interaction/ settings/
|- game/                   current demo client
|  |- application/ player/ scene/
`- main.cpp                game executable composition root
tests/
|- engine/                 reusable subsystem contracts
|- editor/                 authoring behavior
`- game/                   demo integration
```

Long-form project tutorials live under `docs/tutorials/`. The
[first-person game tutorial](tutorials/fps-game/README.md) demonstrates how to
compose the public engine APIs without moving game rules into reusable systems.

Folder-crossing includes use paths rooted at `src/`. CMake source lists are
explicit, so a file move must update `CMakeLists.txt` as well as include sites.

## Build description

`CMakeLists.txt` creates these targets:

| Target | Kind | Responsibility |
| --- | --- | --- |
| `zenith_engine` | Static library | Reusable application lifecycle, scene, asset, audio, input, physics, graphics, and window systems. |
| `zenith_demo` | Static library | Current game composition, scene factory, first-person controller, and gameplay input mapping. |
| `zenith` | Executable | Thin composition root that initializes `Engine` and runs `DemoApplication`. |
| `zenith_editor_app` | Static library | Editor lifecycle, documents, history, interaction, and settings. |
| `zenith_editor` | Executable | Thin editor composition root. |
| `zenith_shaders` | Custom target | Recursively finds recognized GLSL stages in `shaders/` and compiles each file with glslang for Vulkan 1.4. |
| `zenith_assets` | Custom target | Copies `assets/` to `build/assets/` so runtime paths do not depend on the process working directory. |
| `zenith_cooked_textures` | Custom target | Runs `zenith_asset_build cook-model-textures` on every `assets/*.gltf`, writing BC7/BC5 KTX2 entries to `build/derived/`. |
| `zenith_bc7enc` | Static library | The bc7enc_rdo BC7/BC5 block encoders and decoders, linked privately into the engine for texture cooking. |
| `zenith_asset_tests` | Test executable | Asset importing and scene hierarchy behavior. |
| `zenith_physics_tests` | Test executable | Rigid bodies, contacts, impulses, raycasts, mesh collision, and cleanup. |
| `zenith_application_tests` | Test executable | Lifecycle ordering, deferred switching, failed-load cleanup, demo composition, controls, and reload behavior. |

The build publishes `src/` as the engine include root. Consumers include
headers as, for example, `engine/scene/Scene.hpp`. Runtime code uses the
executable-relative resource locator, which supports both multi-config build
trees and cleanly extracted release archives without embedding build paths.

Third-party libraries are pinned with CMake `FetchContent`:

| Dependency | Role |
| --- | --- |
| Vulkan Headers + Volk | Vulkan declarations and dynamic loader. |
| Vulkan Memory Allocator (VMA) | GPU buffer and image memory allocation. |
| SDL 3 | Window creation, Vulkan surface creation, events, keyboard, and mouse. |
| GLM | Vectors, matrices, quaternions, and transforms. |
| EnTT | Entity-component registry. |
| Jolt Physics | Collision detection and rigid-body simulation behind Zenith's physics API. |
| cgltf | `.gltf`/`.glb` parsing. |
| stb_image | Image decoding to RGBA pixels. |
| bc7enc_rdo | Offline BC7 (bc7enc) and BC5 (rgbcx) block compression for cooked textures. |
| glslang | Build-time GLSL-to-SPIR-V compiler. |
| shaderc + SPIR-V Tools | In-process shader compiler dependencies available to the engine target. |

## Application and engine orchestration

### `src/main.cpp`

The composition root. It constructs `Engine` and `DemoApplication`, initializes
the engine, runs the application, and shuts down in reverse dependency order.

### `src/engine/Engine.hpp` and `Engine.cpp`

`Engine` owns the reusable window, input, renderer, assets, scene, and physics
instances, plus audio output and voice state. `run(Application&)` creates a
testable `ApplicationSession`, drives
variable and fixed scene callbacks, renders the active scene, and applies
requested transitions after presentation.

The engine loop polls input once per rendered frame, applies view input, runs
zero or more fixed physics steps, interpolates visible transforms, renders, and
presents. It also implements bounded smoke runs and rendering benchmarks via
environment variables.

## Window and input

### `src/engine/window/Window.*`

A small SDL window owner. `init` starts SDL video and creates a resizable Vulkan
window at 1280x720. `shutdown` destroys the window and quits SDL. Vulkan uses
surface capabilities and current pixel size when selecting its actual extent.

### `src/engine/input/InputManager.*`

Translates SDL-specific state into device-independent held/pressed keys,
mouse movement, gamepad buttons and axes, active-device, and capture state.
Press and release edges are buffered until the first fixed update consumes
them, so short inputs are not lost between physics ticks. Controller connection
changes release held controls rather than leaving stuck state. It also reports
quit and resize requests. Escape releases relative mouse mode; clicking the
window captures it.

### `src/engine/input/InputActions.*`

Maps physical inputs to named button, one-dimensional, and two-dimensional
actions. Bindings support keyboard chords, mouse deltas and wheels, gamepad
dead zones, multiple contributing controls, and runtime rebinding. Gameplay
owns action names and default bindings; the engine does not assign meaning to
specific controls.

## Runtime game UI

### `src/engine/ui/GameUi.*`

Provides a resolution-independent command API for runtime menus and HUDs.
Applications receive a `GameUiContext` through `ApplicationContext` and emit
panels, text, images, and buttons using a 1920x1080 design canvas. The context
owns anchor resolution, uniform scaling, clipping, pointer hit testing, focus,
and action-driven navigation. Game code exchanges only engine handles and
domain types; the renderer privately translates commands to its ImGui Vulkan
backend after tone mapping.

Font files are cached as CPU assets through `AssetManager::loadFont`, while UI
images reuse normal `TextureHandle` values and the renderer's texture cache.

`Localization.*` owns stable message IDs, locale/plural selection, and UTF-8
fallback shaping. `Accessibility.*` owns timed caption tracks and
color-independent indicator semantics. `InputProfiles.*` stores named,
remappable keyboard, mouse, and gamepad profiles with explicit conflict and
hold/toggle policies.

## Audio

### `src/engine/audio/AudioSystem.*` and `StbVorbis.cpp`

Owns SDL output, clip decoding and caching, streamed Ogg decoding, voice
lifetime, bus gain/muting, and scene spatialization. Public clip, stream, and
voice handles are generational; ECS audio components remain declarative.
`StbVorbis.cpp` contains the isolated third-party decoder implementation.

## Scene and gameplay composition

### `src/engine/scene/Scene.*`

Wraps an EnTT registry and defines the core ECS components:

| Component | Meaning |
| --- | --- |
| `NameComponent` | Human-readable entity label. |
| `TransformComponent` | Local transform relative to a parent and derived world transform. |
| `RelationshipComponent` | Parent entity plus an owned list of child entities. |
| `ModelComponent` | Records the model instantiated at a root entity. |
| `MeshRendererComponent` | Refers to CPU mesh data through a `MeshHandle`. |
| `CameraComponent` | Perspective projection settings and primary-camera selection. |

`createEntity` always installs name, transform, and relationship components.
`setParent` updates both sides of a relationship and rejects self-parenting and
cycles. `instantiateModel` expands imported nodes into entities while preserving
their hierarchy. `destroyEntity` recursively destroys descendants.
`updateWorldTransforms` walks roots and computes `world = parent.world * local`.

### `src/engine/scene/Prefab.*`

Defines versioned reusable entity documents with stable local IDs,
project-relative model and nested-prefab references, explicit component
payloads, and separate instance overrides. Nested expansion uses deterministic
instance-derived IDs, rejects source and hierarchy cycles, and validates the
complete graph before mutating a destination scene. `PrefabSystem` is runtime
code, so primary and additive scenes can share definitions without editor
state.

### `src/engine/application/Application.*`

Defines `Application`, `ApplicationScene`, contexts, deferred scene/quit
requests, and `ApplicationSession`. The session guarantees load/update/fixed
update/unload ordering and clears ECS/physics state between scenes while assets
remain cached.

### `src/engine/jobs/JobSystem.*` and `scene/AdditiveSceneManager.*`

`JobSystem` runs bounded prioritized work with cancellation and filtered
completion queues. `SceneDocument` separates background-safe parsing from
main-thread ECS and asset commits. `AdditiveSceneManager` composes those pieces
into stable scene instances with per-instance asset leases, desired-set
streaming, deterministic unload, and validated cross-instance entity
references.

### `src/game/application/`, `scene/`, and `player/`

`DemoApplication` maps its typed scene enum to engine `SceneKey` values.
`GameplayScene` loads the room, creates props and the player, and requests a
fresh gameplay scene through the Reload action. It also composes the title
screen, HUD, pause menu, and persisted audio, rendering, and input settings
with the engine game-UI API. `FirstPersonController` interprets semantic
movement and view input and remains entirely outside `zenith_engine`.

## Projects

### `src/engine/project/ProjectConfig.*`

Defines the shared versioned project manifest used by both game and editor
applications. Version 3 stores a display name, project-relative asset and scene
roots, a scene-root-relative startup scene, and validated renderer quality and
per-pass budget settings. Versions 1 and 2 remain readable and are upgraded on
save. Scene references are validated against the scene root before
`SceneDocument` loads them.

The `zenith` executable accepts `--project <project-file>`. The demo application
loads authored scene content through this engine contract, then adds its
game-specific player and behavior in `src/game/`.

## Editor

`src/editor/main.cpp` is the editor composition root. `application/` owns the
editor lifecycle and session coordinator; `document/` owns scene file state and
command history; `interaction/` owns camera, picking, gizmos, and component
widgets; and `settings/` owns persisted editor preferences. The editor depends
on the engine, while neither the engine nor demo depends on editor code.

## Assets

### `src/engine/assets/Assets.*`

Defines renderer-independent CPU asset types and opaque typed handles:

- `ModelAsset` is a flat list of named nodes with local transforms, parent
  indices, and optional mesh handles.
- `MeshAsset` owns vertices, indices, and material-bearing draw primitives.
- `MaterialAsset` holds glTF metallic-roughness factors and texture handles.
- `TextureAsset` stores width, height, a `TextureUsage` (color, normal, or
  data) derived from referencing material slots, and either an RGBA8 base level
  or a complete BC7/BC5 mip chain.

`AssetManager` owns these values in recyclable generational slots. It normalizes
model paths, caches repeated loads, and resolves handles without exposing its
vectors. Scene sessions, editor operations, audio voices, and renderer frame
slots retain explicit leases. Deterministic collection reclaims unreferenced
assets under configurable CPU and decoded-audio budgets; recycled slots advance
their generation so stale handles cannot alias new content. Renderer frame
leases defer GPU cache pruning until submitted work has retired.

### `src/engine/assets/AssetStreaming.*`

Owns deterministic derived-asset residency policy without importing Vulkan.
It tracks independent CPU/GPU generations, ranks requests by visibility,
distance, gameplay importance, and deadline, and emits bounded decode and
upload work lists. Texture mip ranges and mesh chunks are independent segments.
Ordered pressure handling discards temporary caches, removes optional distant
mips, retires unused generations after a completion serial, then reports a hard
budget failure if live content still cannot fit.

### `src/engine/assets/ModelImporter.*`

Uses cgltf to parse `.gltf` and `.glb`, load buffers, validate the document, and
convert supported nodes, meshes, primitives, materials, textures, and transforms
into Zenith types. It handles indexed and non-indexed triangle geometry and
generates normals or tangent space when absent. Non-triangle primitives are
skipped. Morph deltas and animation weight channels are retained, while
compressed paths report the missing optional decoder by name. Parsing,
resource-limit, and validation failures return descriptive errors.

### `src/engine/assets/ImageDecoder.*`

Uses stb_image to decode conventional external or embedded images and validates
KTX2 containers. RGBA8 KTX2 has a CPU fallback and BC5/BC7 KTX2 loads its
stored mip chain; Basis data reports the optional transcoder requirement. GPU
format policy and upload remain renderer-owned.

### `src/engine/assets/CompressedTexture.*`

Block-compressed texture layout (mip extents and sizes), the BC5/BC7 KTX2
writer and reader, and the cooked-texture convention: a cooked entry is a
`DerivedAssetCache` file keyed by the source image bytes and
`cookedTextureRequest()` settings. `loadCookedTexture()` returns nothing for a
missing entry so importers fall back to the source image.

### `src/engine/assets/TextureCooker.*`

Offline cooking used by `zenith_asset_build` and tests. Builds a full mip chain
(sRGB-correct for color, renormalized for normals) and encodes BC7 for color and
data or BC5 for normal maps, plus a CPU block decoder for validation.

### `src/engine/assets/ProductionAssetPipeline.*`

Defines untrusted-content limits, isolated Meshopt/Draco decoder registration,
KTX2 target-format choice, and deterministic derived assets keyed by source,
settings, tool version, and target. Editor inspection and the
`zenith_asset_build` command share this implementation; the command also cooks
single textures (`cook-texture`) or a model's external textures
(`cook-model-textures`).

### `src/engine/platform/PlatformServices.*`

Resolves platform writable locations, performs atomic user-data writes and
backup recovery, writes local-only structured diagnostics, and serializes exact
release provenance.

## Physics

### `src/engine/physics/PhysicsComponents.hpp`

Defines static, kinematic, and dynamic motion; collision layers; box, sphere,
capsule, and triangle-mesh shapes; rigid-body tuning; and first-person character
tuning/state. Primitive dimensions are multiplied by transform scale when the
Jolt shape is created. Triangle meshes reference a `MeshAsset` and are limited
to static bodies.

### `src/engine/physics/PhysicsWorld.*`

Owns Jolt through a private implementation. Each step discovers entities with
transform, collider, and rigid-body components; creates missing bodies; removes
orphaned bodies; moves kinematic bodies from ECS transforms; advances Jolt; and
writes dynamic translation/rotation back while preserving scale. Interpolation
blends previous and current dynamic poses for smooth rendering.

It exposes forces, impulses, velocity, raycasts, and begin/persist/end contacts
in terms of ECS entities. `refreshBody` rebuilds a body after collider or body
settings change. Native Jolt accessors are deliberate advanced escape hatches.

## Graphics

Graphics consumes scene and asset data but does not know gameplay rules:

```text
Renderer
|- VulkanContext
|- Surface
|- Swapchain (presentation images, depth image, presentation semaphores)
|- FrameResources[3] (command pool/buffer, acquire semaphore, fence)
|- RenderGraph (pass hazards, synchronization plan, transient lifetimes)
`- MeshPass
   |- GPU mesh/texture/material caches
   |- shadow pass and main mesh pipeline
   `- PostProcessPass (HDR/history, motion, SSAO/SSR, and fullscreen pipelines)
```

### `src/engine/graphics/Renderer.*`

Coordinates initialization, per-frame work, resize recovery, and shutdown. A
frame waits for its reusable slot, acquires a swapchain image, records mesh and
post-processing commands, submits them, and presents. Out-of-date/suboptimal
surfaces trigger recreation; zero-sized surfaces are skipped until usable.

### `src/engine/graphics/vulkan/VulkanContext.*`

Owns the Vulkan instance, optional validation messenger, selected physical
device, logical device, graphics/presentation queue, VMA allocator, and Vulkan
pipeline cache. Device selection checks the required Vulkan features and
presentation support. Compatible cache bytes are loaded from the platform user
data directory and atomically persisted before device destruction.

### `src/engine/graphics/surface/Surface.*`

Owns the SDL-created `VkSurfaceKHR` and queries capabilities, formats,
presentation modes, queue presentation support, and format support.

### `src/engine/graphics/swapchain/Swapchain.*`

Chooses extent, BGRA8 sRGB output, a supported depth format, and presentation
mode. It owns the swapchain, image views, per-image render-complete semaphores,
and a VMA-backed depth image/view. FIFO is the portable fallback.

### `src/engine/graphics/resources/FrameResources.*`

Owns one frame slot's command pool, command buffer, image-acquired semaphore,
completion fence, and timestamp queries. Three instances prevent reuse while
the GPU is working. Timeline waits are attached only to stages that consume
uploaded buffers or textures.

### `src/engine/graphics/resources/UploadContext.*`

Owns a bounded three-slot staging ring. Timeline-capable queues are polled for
retirement without blocking; if every slot is still in flight, staging reports
backpressure so the caller can defer work. Fence retirement is the correctness
fallback on devices without timeline semaphores. Timestamp and byte counters
remain attributable to retired uploads.

### `src/engine/graphics/resources/BindlessTextureTable.*`

CPU bookkeeping for the bindless texture array: stable slot assignment, reuse,
and a per-frame dirty mask so each frame slot's descriptor set is rewritten only
for slots that changed. Released slots fall back to a default view.

### `src/engine/graphics/resources/GeometryArena.*`

Suballocates meshes into shared vertex/index pages with VMA virtual blocks,
opening a new page (sized to fit oversized meshes) when none has room. It owns
no Vulkan buffers; the mesh pass creates one buffer pair per reported page.

### `src/engine/graphics/passes/GpuDrivenPass.*`

Two-phase GPU frustum and occlusion culling, the min-depth pyramid, and
draw compaction into indirect-count commands. The caller owns render passes
and the scene-depth transitions around the pyramid build.

### `src/engine/graphics/graph/RenderGraph.*`

Compiles imported and transient resource declarations into a stable pass order,
synchronization2 hazard plan, queue transfers, and compatible lifetime aliases.
Its statistics report requested, committed, aliased, and peak transient bytes;
debug builds compare the result with conservative synchronization.

### `src/engine/graphics/shader/ShaderModule.*`

A short-lived RAII wrapper that reads SPIR-V and creates a `VkShaderModule` for
pipeline creation. Its destructor releases the module.

### `src/engine/graphics/pipeline/GraphicsPipeline.*`

Builds and owns the main mesh or depth-only shadow pipeline and layout using
Vulkan dynamic rendering.

### `src/engine/graphics/passes/MeshPass.*`

Finds the primary camera, updates per-frame camera data, lazily uploads CPU
assets to renderer-owned GPU caches, and publishes bindless material records and
texture slots. It first
renders a directional shadow map and then the scene into a floating-point HDR
target using Cook-Torrance GGX lighting plus hemispherical sky/ground indirect
lighting. Directional and environment scene components supply lighting and
shadow settings. Fallback textures fill missing maps.

Local point and spot lights are selected through deterministic 16-pixel tile
coverage with a saved 32- or 64-light bound. Environment textures use cached
roughness-prefiltered data, while reflection probes contribute through explicit
influence boxes, blend distances, and priorities.

### `src/engine/graphics/passes/PostProcessPass.*`

Owns one RGBA16F history target, motion/reactive target, SSAO target, and SSR
target per frame slot. Separate GPU-scoped SSAO and SSR passes feed the final
fullscreen pass, which performs neighborhood-clipped TAA (or FXAA), bloom,
editor-grid composition, and ACES tone mapping. Half-resolution effect tiers
render into bounded sub-extents; SSR misses preserve probe and IBL lighting.

## Shaders

| File | Stage and job |
| --- | --- |
| `shaders/mesh.vert` | Transforms mesh vertices and supplies world-space attributes. |
| `shaders/mesh.frag` | Samples glTF materials, evaluates GGX lighting, shadows, occlusion, and emission. |
| `shaders/shadow.vert` | Transforms geometry into directional-light depth space. |
| `shaders/fullscreen.vert` | Generates a fullscreen triangle without a vertex buffer. |
| `shaders/post.frag` | Combines HDR color into the tone-mapped output. |
| `shaders/ssao.frag` | Estimates scalable screen-space ambient occlusion. |
| `shaders/ssr.frag` | Traces scalable screen-space reflections with an explicit miss. |
| `shaders/depth_pyramid.comp` | Reduces scene depth into a conservative min-depth (reversed-Z) pyramid. |
| `shaders/gpu_visibility.comp` | Runs early/late two-phase frustum and occlusion culling. |
| `shaders/compact_draws.comp` | Packs non-empty batches into indirect-count draw commands. |

Recognized shader extensions automatically produce matching
`build/shaders/<relative-path>.spv` files.

## Runtime content

| Path | Purpose |
| --- | --- |
| `assets/sample.gltf` | Small importer and static-mesh-collider fixture. |
| `assets/basic_map.gltf` | Current room, materials, and prop geometry. |
| `assets/audio/*` | Demo interaction and ambient audio plus provenance notes. |
| `assets/textures/*.png` | Base-color, normal, and packed occlusion/roughness/metallic maps. |
| `assets/textures/SOURCE.md` | Texture provenance and channel/convention notes. |

The source directory is copied as a whole to `build/assets/` for runtime use.

## Tests

| Path | Coverage |
| --- | --- |
| `tests/engine/` | Audio, asset importing, scene documents/hierarchy, and physics contracts. |
| `tests/editor/` | Settings, document state, command history, camera behavior, and component editing helpers. |
| `tests/game/` | Application callback order, scene changes, cleanup, demo composition, controls, and reload behavior. |

The executables return a nonzero status on failed expectations so CTest can
report failures without another test framework. Test sources and registrations
are explicit in `CMakeLists.txt`.
