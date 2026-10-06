# Architecture

Zenith is organized as a reusable engine with separate game and editor
applications. Lower-level systems provide mechanisms; applications choose the
current content, behavior, and authoring policy.

This page is the overview: dependency direction, ownership, the startup and
frame sequences, and a short summary of each data flow. Each summary links to
the subsystem page that owns the contract; the full list of subsystem pages is
in the [repository guide](REPOSITORY_GUIDE.md#subsystem-docs).

## Dependency direction

```text
main.cpp ------> DemoApplication|ProjectApplication --+
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
- `EditorSession` owns editor-only UI and persistence state and composes the
  dockable panels in `src/editor/panels/`. Panels see the session only through
  a per-frame `EditorContext` and change the scene only through undoable
  `SceneEditing` commands. Each command stores the document form of only the
  entities an edit touched (and the collection definitions when it changed
  them) and undoes or redoes it by patching them in place
  (`SceneDocument::diff` and `applyChanges`), so untouched entities keep their
  EnTT handles; selections and other long-lived references still use stable
  `SceneEntityId`s because restored entities receive new handles. Patched
  entities are placed among their siblings in stable-id order, which is the
  order a full document load produces. Workspaces, their areas (editor-type
  windows, see `src/editor/areas/`), each area's view state (such as the
  Properties tab), and each workspace's docking layout are user data in
  `workspaces.json`, stored separately from runtime scene data,
  so changing the workspace cannot mutate the scene.

## Shared engine library and game modules

`zenith_engine` is a shared library, so the editor, the game executable,
tests, and runtime-loaded game modules share one copy of engine code and of
third-party state that must not be duplicated (SDL, the ImGui context, volk,
Jolt's allocator and factory, shaderc). A game module is a shared library with
C-linkage entry points that registers component types and systems into a
host; a `SystemScheduler` runs those systems per phase (Update, FixedUpdate,
LateUpdate, Ui). A host releases everything a module registered before
unloading it. The export rules, the module API, the frame order, and hot
reload are in [Game modules](engine/modules.md); editor hosting is in
[Game module hosting](editor/modules.md) and editor extensions in
[Editor extensions](EDITOR_EXTENSIONS.md).

## Ownership and lifetime

`main` owns, in declaration order, the `RuntimeGameplay` (the project's game
module and what it registered), `Engine`, and the application, so the
module's code outlives the engine's scene. The engine owns subsystems by value.
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
  -> parse the command line; load the project and its game module
  -> Engine::run(DemoApplication or ProjectApplication)
       -> Application::onStart
       -> create initial ApplicationScene
       -> ApplicationScene::load
            -> import/cache model assets
            -> instantiate ECS hierarchy
            -> add collision, bodies, controller, and camera
```

Recoverable stages return `bool` or a result structure containing an error.
`main` reports failure and still calls shutdown. The command line and what a
run plays are in [Application](engine/application.md).

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
       ApplicationScene::afterPhysicsStep
       consume buffered key presses
  -> clamp excessive backlog
  -> interpolate dynamic body transforms
  -> ApplicationScene::lateUpdate
  -> update scene transforms and render the HDR scene
  -> tone map, compose game UI, and present
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

The application's simulation clock (time scale, pause, frame step) scales
fixed-step accumulation, animation, and scene audio, and the engine records
per-phase CPU timings each frame; see
[Application](engine/application.md#srcengineapplicationsimulationclockhpp-and-frame-timings).
Where game systems run within the frame is in
[Game modules](engine/modules.md#frame-order-and-phases).

## Data flows

### Projects and authored scenes

Both applications consume the engine-owned `ProjectConfig` contract: a
versioned manifest with project-relative asset and scene roots, the startup
scene, renderer quality and budgets, an optional game module, and the
project's physics layers, input actions, tags, and navigation settings (see
[Projects](engine/projects.md)). The game executable chooses what plays (a
project's module, or the demo gameplay linked into it) and which application
presents it; see [Application](engine/application.md) and
[Demo game](game/demo.md).

### Scene documents, additive scenes, and prefabs

Scene documents are driven by a `ComponentTypeRegistry`, so the document
format, the inspector, and game-defined components share one description;
unknown component keys are preserved, and document-level migrations upgrade
older files. Background jobs parse; the main thread commits, so a failed
commit cannot publish a partially loaded additive scene.
`AdditiveSceneManager` streams a desired set of scene instances with
per-instance asset leases. `PrefabSystem` is runtime code that instantiates
versioned prefab definitions into any scene, tracks field-level overrides, and
remaps entity references. See [Scene](engine/scene.md); the editor's prefab
operators and their undo behavior are in [Operators](editor/operators.md).

### Entities and transforms

`Scene` is an EnTT registry plus hierarchy operations. Every entity starts
with identity transforms and a relationship component, and components are
capabilities (renderable, camera, light, physics body, character). Zenith uses
GLM column-major matrices composed as `parentWorld * local`, +Y up, and
first-person forward toward -Z. See
[Scene](engine/scene.md#entity-invariants-and-component-capabilities).

### Assets

`AssetManager` parses files into CPU values in generational slots and hands
typed handles to components; `MeshPass` uploads a handle's data to the GPU on
first use. Reload is transactional, collection respects leases and budgets,
textures are cooked offline to BC5/BC7 and stream their mips, and
`ResourceLocator` resolves asset and shader roots from the executable path.
See [Assets](engine/assets.md#asset-flow).

### Physics

At the beginning of a step `PhysicsWorld` discovers entities with transform,
collider, and rigid-body components, creates and removes Jolt bodies to match,
submits kinematic poses, advances Jolt, and copies dynamic poses back. Layers
come from the project's `PhysicsLayerTable`; queries take a layer filter;
sensor overlaps become trigger events. See
[Physics](engine/physics.md#srcenginephysicsphysicsworld).

### Runtime game UI

`Engine` owns one `GameUiContext`. Game code records panels, text, images,
and widgets on a 1920x1080 design canvas without ImGui or Vulkan headers, and
`UiPass` composes them after tone mapping. Localization, accessibility, and
input profiles stay below game policy. See [Runtime game UI](engine/ui.md).

### Rendering

Each frame slot waits on its fence, acquires a presentation image, and records
one command buffer: shared work (environment precompute, shadow cascades,
uploads) once, then each render view's clusters, prepass, depth pyramid, AO,
shading, reflections, blended surfaces, particles, temporal resolve, depth of
field, motion blur, exposure, bloom, tone mapping, colour grade, debug lines,
and UI. A render-graph model mirrors the pass list and validates its hazards.
The ordered pass list, synchronization, and settings are in
[Graphics](engine/graphics.md#render-flow); the passes are in
[Graphics scene passes](engine/graphics-passes.md) and
[Graphics post-processing](engine/graphics-post.md); render views, secondary
scenes, and GPU picking are in [Graphics render views and picking](engine/graphics-views.md).
The editor reaches the renderer only through `RenderRuntime` requests and
opaque texture handles ([Viewports](editor/viewport.md#renderer-contract)).
How renderer changes are measured and validated is in
[Renderer validation](RENDERER_VALIDATION.md).

## Error model

Subsystems that can provide useful diagnostics return small result values:
`SceneResult`, `AssetLoadResult`, and `PhysicsResult`. Low-level Vulkan helpers
return `bool` after reporting the failed operation. Invalid asset handles
resolve to `nullptr`; action-style physics calls return `false` when no managed
body exists. Handle outcomes where the caller can add context or recover.
