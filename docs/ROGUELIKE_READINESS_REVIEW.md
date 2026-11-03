# Roguelike readiness review

Reviewed 2026-10-04 at commit `0d0b81a`. Line references are to that commit and will
drift; the file and symbol names are the durable part.

## Context

A procedurally generated first-person roguelike is the next game on Zenith (engine `src/engine/`, Blender-style editor `src/editor/`, C++ game modules hot-reloaded as DLLs). Phase E's "roguelike foundations" are done:
- seeded RNG streams;
- run and profile saves;
- `DefinitionTable`;
- floor build and teardown;
- nav partial paths and crowds;
- gameplay audio;
- pause and time scale.

This is a critical review of the editor workflow, the engine runtime, and differentiation opportunities, prioritised so that game development won't keep stopping to repair foundations.

**Method.** Three read-only surveys covered the editor, the runtime, and roguelike/procgen readiness. The highest-impact claims were then checked against the source by hand (see [Verification](#verification)). Every finding below cites the code.

**Status (2026-10-05).** Every CRITICAL item is implemented, with tests, on master (merges `10765c0` through `307d354`; module API version 9). Each item's subsystem doc describes what was built, and the remaining follow-ups are listed under [Critical follow-ups](#critical-follow-ups). The other tiers are still open.

---

## Overall verdict

Zenith is not a prototype. It has:
- a modern GPU-driven renderer;
- generational asset handles with leases;
- prefabs with field overrides;
- an operator/keymap/menu system;
- reflection-driven inspection;
- Play/Simulate with live editing;
- hot-reloaded C++ modules;
- around 900 tracked source and test files;
- no TODO or FIXME debt.

The architecture is clean, and its dependency direction holds.

The weaknesses cluster in three places, and all three matter for a procedural game:

1. **The runtime assumes content arrives as imported glTF or authored documents.**
   - Code can't create a mesh, material or texture (`AssetManager::addMesh/addMaterial/addTexture` are private, `src/engine/assets/Assets.hpp:497-500`).
   - Every spawn goes through JSON text (`PrefabSystem.cpp:177-192`, which builds twice; each build re-reads the file from disk).
   - A roguelike is mostly code assembling content at runtime.

2. **Scale and failure behaviour are tuned for hand-authored demo scenes.** Several limits end the game rather than degrading:
   - More than 16,384 renderables makes `MeshPassRecord.cpp:477` return false. `drawFrame` then fails and `Engine.cpp:290` exits the main loop.
   - Any physics step error, including Jolt cache overflow or one malformed body, sets `running = false` (`Engine.cpp:238-243`).
   - Only Behaviour hooks are exception-guarded. A throwing system (`SystemScheduler.cpp:154-167`), timer, or command-buffer callback takes the whole editor down.
   - There is no SEH or minidump handler.

3. **The editor is strong at editing *a scene* and weak at authoring *content* and *generation*:**
   - texture, audio and mesh fields are read-only strings (`ReflectedFieldEditor.cpp:476`);
   - there is no material asset;
   - there is no prefab edit mode;
   - the Animator has no editor;
   - there is nothing for previewing a seed in Edit mode.

   Every undoable operator, duplicate, paste and Play entry serializes the whole scene to JSON, several times for duplicate and paste.

None of this calls for a redesign. Most fixes are additive, and they reuse mechanisms that already exist: `captureEntities`, `RandomStreams::snapshot`, `GameplayRunner`, `EntityCommandBuffer`, module editor extensions.

---

## Part 1 — Editor workflow

The workflows below become painful when building this specific game. Each one names the friction it causes.

### 1.1 Authoring room prefabs (the roguelike's core content loop)
Content will be dozens of room, corridor and prop prefabs with door sockets.

What exists:
- `prefab.create_from_selection`, instantiate, apply, revert, revert_field and unpack (`src/editor/operators/PrefabOperators.cpp:222-236`);
- override highlighting.

What's missing:
- **No prefab edit mode.** You can't open `room_07.prefab` in isolation. You have to instantiate it into some scene, edit it, Apply, then delete the instance.
- **Apply writes the `.prefab` file outside undo** (`src/editor/document/PrefabAuthoring.hpp:73-77`). Ctrl+Z restores the scene but not the file, so a bad Apply is permanent.
- **Apply refreshes only instances in the open scene** (`:89-92`).
- **No variants**, such as "room_07 but flooded".
- **No socket or connector concept**, so door positions and facings must be encoded as named empties and found by walking `RelationshipComponent`. The engine has no `findChild` or `findDescendant`.

*Workflow pain:* every room tweak is a five-step dance with an irreversible write, and the generator's contract with each room (where its doors are) is a naming convention.

### 1.2 Assigning assets and materials
- **Asset fields are read-only text.** Texture-backed fields are drawn with `drawReadOnlyText` (`src/editor/interaction/ReflectedFieldEditor.cpp:476-480`):
  - environment HDR (`LightComponentTypes.cpp:124`);
  - probe environment;
  - decal albedo and normal;
  - particle sprite.
- **Audio clips are typed paths** (`AudioComponentTypes.cpp:29-32`).
- **Model and Mesh Renderer are `.internal()`** (`RenderComponentTypes.cpp:30,79`), so you can't swap the mesh an entity uses.
- **No asset picker, and no drop target on fields.** The only inspector drop targets are entity references and "Drop Model Asset to Add Child" (`EntityPropertiesView.cpp:828-836`).
- **No material asset or editor at all.** Materials exist only inside imported glTF. There is no per-instance tint or override component either; `MeshRendererComponent` holds just a `MeshHandle` (`Scene.hpp:98-100`).

*Workflow pain:* making a red "danger" variant of a crate, retexturing a greybox wall, or giving a decal a texture all mean a Blender round-trip or hand-editing JSON. Greyboxing, which is what the next milestone is, wants fast colour and material iteration.

### 1.3 Placing and arranging things
What's good:
- 3D cursor, surface and vertex snap, box and lasso select, modal G/R/S, pie menus.

What's missing:
- **The gizmo appears only with exactly one entity selected** (`ViewportPanel.cpp:1052`). Multi-object moves need modal G/R/S, which is Edit-only (`EditorObjectCommands.cpp:198`).
- **No align-to-surface-normal.** Snapping sets translation only.
- **Asset drops land at the 3D cursor, not under the mouse** (`EditorContext.cpp:16`).
- **The Add menu offers only Cube, Sun, Environment, Fog and Empty** (`DefaultMenus.cpp:66-70`). Point lights, spot lights and cameras go through "Empty with Component".
- **No array or scatter, select-similar or select-children, or align/distribute.** Unparenting works only by drag.

*Workflow pain:* dressing a room with 30 props is noticeably slower than in Unity or Godot.

### 1.4 Collision and navigation editing
- **The collider editor can't change shape type** (`ComponentEditors.cpp:67-85`).
- **`ColliderComponent` has no local offset** (`PhysicsComponents.hpp:65-67`). With one collider per entity, a door frame needs child entities.
- **Collider gizmos are draw-only**, with no handles. Collision matrix edits are not undoable.
- **`navigation.bake` has no settings UI.** Agent radius, height and climb (`NavigationWorld.hpp:17-25`) aren't exposed, the bake isn't in any menu, and the navmesh overlay can't be toggled (`ViewportPanel.cpp:452`).
- **Play mode has no Jolt debug draw.** Overlays are rebuilt from components, so contacts, sleeping bodies and character controllers can't be seen live.

*Workflow pain:* "why can't the enemy reach me?" has no visual answer: no live nav, crowd or physics overlay during play.

### 1.5 Animation
- **The Animator state machine has no editor at all.** It's a codec-only component with no reflected fields (`AnimationComponentTypes.cpp:216-229`).
- States, transitions, parameters and events can only be authored in raw scene JSON or C++.
- The Animation component gives only a clip combo, a scrubber and Preview (`ComponentEditors.cpp:87-116`).

*Workflow pain:* every enemy (idle, chase, attack, hit, die) is authored blind.

### 1.6 Undo, duplicate, and Play-entry cost
**Whole-scene JSON on most edits:**
- `OperatorRunner.cpp:76-99` captures the whole scene before and after every `OperatorFlags::Undo` operator, even in Play, where nothing is recorded.
- Adjust Last Operation captures again.
- Duplicate and paste go further. `SceneFragment::copy` serializes the whole scene to extract the selection (`SceneFragment.cpp:72-76`). `paste` re-serializes, merges, deserializes into a staging scene, and calls `replaceWith` (`:160-206`). That's about five full serializations plus a rebuild per Shift+D.
- In Play, the `replaceWith` drops runtime physics state.

**Play entry:**
- Play entry does `serialize` then `deserialize` (`EditorPlayMode.cpp:10-16`).

**The cheap path already exists but is barely used:**
- Entity-scoped `captureEntities` exists and is used only by the inspector and gizmo (`EntityPropertiesView.cpp:250,430,724`; `ViewportPanel.cpp:830,1132`).

*Workflow pain:* small today. It grows with every authored hub or set-piece scene and is what makes the editor "feel heavy" later. The NEXT_STEPS test follow-up already notes it (since done; see [Completed milestones](history/milestones.md#test-suite-follow-ups-completed)).

### 1.7 Assets and import
- **No import action and no OS file drop.** You copy files into the folder by hand.
- **No import-settings UI.** `AssetDatabase::setImportSettings` (`AssetDatabase.hpp:130`) is never called from the editor.
- **No reload for textures, audio or shaders.** Only models reload.
- **Thumbnails are decoded but never drawn.** Tiles show only icons (`AssetBrowserPanel.cpp:676-690`).
- **No "find references".** Deleting an asset doesn't check what still uses it.
- **Asset kinds stop at Model, Texture, Audio, Scene, Prefab and Script** (`AssetCatalog.hpp:14-22`). There are no data or definition-table assets, even though `DefinitionTable` JSON will be a primary content type.

### 1.8 Debugging, visualisation, and Play-in-editor
What's good:
- Info log with filters; System Profiler with per-system CPU and per-pass GPU timings; Watch fields; Attach Debugger; live inspector during Play; Apply-to-Edit.

What's missing:
- **`DebugDraw` lasts one frame only** (`Rendering.hpp:876-881`). There are no durations, persistent layers or channels.
- **Labels render only in editor viewports** (`ViewportPanel.cpp:468`), not in `zenith --project`. Tutorial 06 claims otherwise.
- **No debug draw in Edit mode.** `services` and `application` are null there (`GameModuleHost.cpp:428,494`), and `EditorModulePanelContext` has no `DebugDraw`.
- **No overlay toggles** for physics, nav or crowd during Play.
- **No crash safety.** A throwing system or timer, or any access violation, ends the editor process. The only safety net is the autosave recovery file.

### 1.9 Procedural-generation tooling (absent)
**Nothing in `src/editor` is aware of seeds or generators.** These are the hooks it would build on:
- **Module operators:** they take no properties (`EditorModuleApi.hpp:77-90`, and the wrapper ignores `OperatorProperties` at `GameModuleHost.cpp:62`), so a "Generate (seed)" operator can't expose a seed to Adjust Last Operation. They also get only `{Scene&, AssetManager&, selection}`: no RNG, no `PrefabSystem` configured for module components, and no debug draw.
- **`RunIn::Edit` systems:** they mutate the document outside undo.
- **No transient / don't-save entity flag**, so a preview floor would be saved into the scene and into undo history.
- **Today's preview loop:** add a `seed` field to a behaviour, then press F5. Simulate runs only FixedUpdate systems by default (`SystemScheduler.hpp:145-148`).

*Workflow pain:* this is the game's main iteration loop (change a rule, look at seeds 1–20, find a bad one, debug it), and today it's "edit code, Play, walk around".

### 1.10 Large-scene organisation and discoverability
- **The outliner rebuilds the full tree every frame without a list clipper** (`HierarchyPanel.cpp:164`). A generated floor of 2,000+ entities shown during Play will make the editor sluggish.
- **The outliner filter matches names only.** There's no filter by component or tag.
- **Children are sorted by entity id** (`OutlinerTree.cpp:34-47`), so siblings can't be reordered.
- **Collections can't be renamed.**
- **F3 operator search is good.**

### 1.11 Editor architecture callout
`EditorSession` is about 3,600 lines across 15 files, with 81 data members and 6 private command interfaces (`EditorSession.hpp:110-116`). It isn't blocking yet. Any new editor workflow (prefab edit mode, generation preview) should land as its own document- or context-owned object rather than more `EditorSession` members.

---

## Part 2 — Engine capabilities

### 2.1 Should exist before game development starts

| # | Area | Problem (verified) | Why it can't wait |
|---|---|---|---|
| E-1 | **Runtime content creation** | `AssetManager::addMesh/addMaterial/addTexture` are private, with `ModelImporter` the only friend (`Assets.hpp:495-500`). Triangle-mesh colliders need an asset-backed `MeshHandle` (`PhysicsComponents.hpp:59`). | Without it you can't merge room geometry into batched meshes, have procedural walls, floors or stairs, build a minimap mesh, set runtime material tints, or give generated geometry colliders. Retrofitting later changes how the generator emits content (prefab kit only vs geometry). |
| E-2 | **Spawn performance and API** | `PrefabSystem::instantiate` validates by building into a throwaway `Scene`, then builds again (`PrefabSystem.cpp:177-192`). Each `build` re-reads and parses the file (`:241`), converts entities to JSON, dumps to string, and re-parses through `SceneDocument::applyChanges` (`:251,390-393`). That measures about 1.4 ms per room ([gameplay roadmap E4](history/gameplay-roadmap-2026.md#e4-building-and-replacing-a-floor-at-runtime)). `EntityCommandBuffer::spawn` can't pass overrides (`EntityCommandBuffer.cpp:122`). | Floors of rooms plus props plus enemies multiply this. Mid-fight enemy spawns pay it on the main thread. The API shape (overrides, sockets, callbacks) is what gameplay code will be written against. |
| E-3 | **Graceful limits** | The renderable cap of 16,384 fails the frame and exits (`MeshPassInternal.hpp:18`, `MeshPassRecord.cpp:477`, `Engine.cpp:290`). A physics step error exits (`Engine.cpp:238`), including Jolt pair/contact cache overflow (`PhysicsWorld.cpp:938`) and one bad body (`:635-645`). Particles silently cap at 64 emitters (`Particles.hpp:39`). Audio fails `play` at 64 voices with no stealing (`AudioSystem.cpp:460`). | Procgen produces outlier floors. A seed that spawns too much should log, clamp and show in stats, not quit the game. |
| E-4 | **Fault isolation** | `SystemScheduler::run` is unguarded (`SystemScheduler.cpp:154-167`); so are `Timers::advance`, `onSpawned` callbacks (`EntityCommandBuffer.cpp:138`) and `Effects`. The editor play loop has no catch, and there's no SEH handler or minidump. | In a hot-reload C++ workflow, crashes are the most frequent interruption. Stop Play with a diagnostic and keep the editor alive. |
| E-5 | **Transform API safety** | `TransformComponent` is a raw `mat4 local/world` (`Scene.hpp:30-33`), and modules must call `markTransformDirty` after raw edits. The audit that catches a missed mark runs every frame in Debug but every 300 calls in Release (`Scene.cpp:372-377`), so a missed mark looks fine in Debug and lags for seconds in Release. `world` is stale until `updateWorldTransforms()`, which also bites nav bake after a spawn (the floor test calls it manually, `FloorFixture.hpp:151`). | This is the API every gameplay file touches. Changing it after the game has hundreds of call sites is the costliest kind of change. |
| E-6 | **Particle emitter churn** | Any change to the emitter set restarts every emitter in the scene (`Particles.hpp:223-229`, `Effects.hpp:26-29`). | An enemy with a torch spawning resets every fire on the floor. This is a visible correctness bug in normal play. |
| E-7 | **Asset collection at runtime** | `collectUnused` is called only from the editor `DiagnosticsPanel.cpp:128` and a smoke test. `src/game` never collects. CPU mesh data stays resident. | Floors with different tilesets grow memory monotonically over a run. The fix is cheap: collect on floor teardown and on scene change. |
| E-8 | **Deterministic event order** | Jolt contacts are pushed from worker threads under a mutex (`PhysicsWorld.cpp:236`) and published unsorted (`GameServices.cpp:166-169`). Characters iterate an `unordered_map` (`:414`). | Seed + input reproduction is the roguelike's bug-report currency, and later the replay differentiator. Sorting is cheap now, and gameplay will otherwise silently depend on arbitrary order. |

### 2.2 Implement when the game first requires it

- **Navigation.** Possible additions:
  - a tiled navmesh with async per-tile builds;
  - dynamic obstacles (`dtTileCache`) and off-mesh links (jumps, ladders);
  - area costs and query filters (`findPath` has none);
  - multiple agent sizes.

  Today's single tile, synchronous bake (26 ms for 40 rooms) works behind a floor-load screen. **Do one thing now:**
  - benchmark the largest intended floor to confirm it fits in a single tile's polygon and vertex limits;
  - fix the double bake: the lazy bake in `BuiltInSystems.cpp:50-54` fires before the game's explicit bake;
  - fix the stale-transform trap (E-5).

  A crowd reset on rebake is fine while floors are rebuilt whole.
- **Animation.** Possible additions:
  - 1D/2D blend trees for locomotion;
  - layers and masks (upper-body attack);
  - root motion and IK.

  Blend trees are likely the first need, once an enemy has walk and run. Animation also advances on variable time before the fixed step (`Engine.cpp:213`). Revisit that only if animation events drive gameplay timing.
- **Physics.** Possible additions:
  - compound and convex-hull shapes, joints and ragdolls;
  - a shape cache so 500 identical wall meshes don't build 500 BVHs (`PhysicsWorld.cpp:545-588`);
  - teleport without recreating the body (`markPhysicsDirty` → remove and recreate loses velocity);
  - publishing contacts to `EventQueue`, since only the last substep's contacts survive (`:975`).

  A collider **offset** is the exception: do it with the editor collider work (1.4).
- **Audio.** Possible additions:
  - priority and voice stealing;
  - a smooth rolloff curve, since the hard cut at max distance pops (`AudioSystem.cpp:1030`);
  - reverb zones;
  - user buses.

  Voice stealing becomes necessary the first time a fight has 30 enemies.
- **GameUi.** Possible additions: text measure and wrap, scrolling, 9-slice, list layouts. Inventory and item tooltips will need wrap and scroll early.
- **Input.** Possible additions: stacked contexts (gameplay vs menu) with consumption. There's one flat map today (`Gameplay.hpp:52-56`).
- **Jobs for gameplay.** Expose `JobSystem` (deterministic mode exists, `JobSystem.hpp:69`) when generation itself becomes slow. Generation should be pure data in → layout out, so it can move to a worker.
- **Gameplay messaging.** Possible additions:
  - an immediate signal or callback alongside the one-frame-late `EventQueue` (`EventQueue.hpp:29-33`);
  - coroutines or sequences;
  - a spatial index for non-physics entities;
  - an interned tag type instead of scanning and sorting `vector<string>` per query (`Tags.cpp`).
- **Async asset loading and runtime additive scenes.** `AdditiveSceneManager` exists but `Engine` renders one scene (`Engine.cpp:153`). Floors load behind a transition, so streaming isn't needed.
- **Profiling.** Possible additions: scoped CPU markers for gameplay (Tracy-style), a file log sink, and an in-game console or cvars. Markers and a file sink are cheap; add them the first time a frame spike needs explaining.
- **Reflection.** Add `vec2`, `quat`, `int64` and lists of scalars (`Reflection.hpp:19-47`), which save structs and definition tables will want soon. Add them when a struct first needs one.
- **A home for game data.** `DefinitionTable` and run state have no owner in `GameServices`, and module statics are lost on hot reload. Add a typed session resource store (`services.resource<T>()`) plus an asset-root path helper. This is small and needed in week one, so it could reasonably move up to CRITICAL.

### 2.3 Unnecessary or overengineering for this game

- **Wrapping EnTT behind a custom ECS facade.** Instead, commit to EnTT as public API, document it, and add the few safe mutation helpers from E-5.
- **Turning `RenderGraph` into a real executor.** Today it is a planning and statistics model (`graph/RenderGraph.hpp:137-173`). It works; leave it.
- **A GameUi layout editor, a node-graph Animator editor, or multi-scene and additive editing.**
- **Open-world streaming, `JPH_CROSS_PLATFORM_DETERMINISTIC`, networking, or a scripting VM.**
- **Splitting `MeshPass`** (about 6k lines, 93 members) or `Renderer.cpp` (1,693 lines) now. It's a known god-class, but stable and outside the game's path.

---

## Part 3 — Differentiation opportunities

These build directly on mechanisms Zenith already has, which is why they're plausible rather than aspirational.

1. **Generation as an inspectable, staged pipeline (the headline feature).**
   - **Engine:** a small `Generation` API in which a generator is a list of named stages. Each stage is a pure function over a data layout (graph → room placement → connectors → props → spawns → nav) with its own named RNG stream, and each stage's output is kept as an artifact.
   - **Builds on:**
     - `RandomStreams` (named streams, snapshot and restore; `src/engine/gameplay/Random.hpp:103-122`);
     - `DebugDraw`;
     - module editor extensions.
   - **Editor: a Generation panel.** It offers:
     - a seed field, regenerate, and step stage by stage;
     - a per-stage debug overlay;
     - pin a seed;
     - diff two seeds;
     - click an entity to see which stage and rule produced it.

   Unity, Unreal and Godot treat procgen as user code drawing gizmos. Making the *process* inspectable is the distinctive part.

2. **Connector-based kit assembly.**
   - **Engine:** a generic `ConnectorComponent` (socket type tag, facing, size) with editor gizmos, plus an assembler that places prefabs by matching connectors and rejecting overlaps.
   - **Builds on:** `PhysicsWorld::overlap` or AABBs, and fast instantiation (E-2).
   - It suits dungeons, modular buildings and level kits alike, so it belongs in the engine rather than the game. It also fixes 1.1's "doors are a naming convention".

3. **Seed farms: generation as a testable artifact.**
   - **Engine:** a CLI, `zenith --project p --check-seeds 1..1000`. It runs the generator headless through `GameplayRunner` (`src/engine/module/Gameplay.hpp:57`) and asserts game-registered invariants: stairs reachable (nav `findPath` returns Complete), item counts, no overlapping rooms, and limits from E-3.
   - **Output:** a report of failing seeds, each with its stage artifacts, openable in the Generation panel.
   - CI-checked generation is rare and very valuable for a roguelike.

4. **Deterministic replay.**
   - **Engine:** record the per-fixed-tick `InputActionState` plus seed plus build hash, then replay headless or in the editor.
   - **Builds on:**
     - the fixed step and buffered input edges (`Engine.cpp:223-259`);
     - PCG streams;
     - deterministic behaviour order (sorted by scene id);
     - sorted physics events (E-8).
   - **Payoff:** a bug report becomes a 50 KB file, and replays double as regression tests through `GameplayRunner`.

5. **Promote generated → authored.**
   - Bake a seed's floor (marked transient) into an editable scene or prefab, hand-edit it, and save it as a set piece that the generator can then place as a room.
   - **Builds on:** Apply-to-Edit (`src/editor/document/RuntimeApply.cpp`) and prefab authoring.
   - This bridges procedural and authored content with no handoff file format.

6. **Spatial fields: influence maps over the navmesh.**
   - **Engine:** a generic scalar or vector field over nav polygons or a voxel grid (danger, noise, light, scent), with propagation and decay, plus a debug heatmap overlay.
   - It's the data-oriented foundation for the deferred "living dungeon" perception layer (sight, sound and disturbance accumulators), and it's generic AI tooling any game can use.

7. **A procedural greybox kit, once E-1 exists.**
   - Engine-side extrusion helpers produce meshes, colliders and nav input together (box rooms, corridors along a graph, stairs).
   - **Builds on:** `NavigationWorld::bake(const NavigationGeometry&)`, which already accepts raw triangles (`NavigationWorld.hpp:87`).
   - You prototype a floor's shape before any art exists, without Blender.

---

## Prioritised recommendations

### CRITICAL BEFORE GAME DEVELOPMENT
Ordered by rewrite risk. All seven are done (see Status above).

1. **Runtime content API (E-1).**
   - **Content creation:**
     - add public `AssetManager::createMesh(MeshAsset, debugName)`, `createMaterial`, and `createTexture`, returning leased generational handles with no source path, collected like any other asset;
     - a `MaterialOverrideComponent` (base colour, emissive, roughness, texture swap) handled by the mesh pass.
   - **Collision and navigation:**
     - `TriangleMeshCollider` accepts those handles;
     - a Jolt shape cache keyed by mesh handle and generation;
     - `GameServices::bakeNavigation` also accepts `NavigationGeometry`.
   - **Files:** `src/engine/assets/Assets.hpp`, `Assets.cpp`, `PhysicsWorld.cpp` (`createShape`), `MeshPass*`, `GameServices.hpp`.
2. **Fast, override-capable spawning (E-2).**
   - **Caching:**
     - a parsed `PrefabDocument` cache in `PrefabSystem`, keyed by path and write time, and invalidated by the asset watcher;
     - validate once per cached document rather than per instance.
   - **Building:** build entities directly from the parsed document through component codecs, not via JSON text → `applyChanges`.
   - **Command buffer:** `EntityCommandBuffer::spawn` takes overrides.
   - **Lookup:** add `Scene::findChild(entity, name)` / `findDescendant`.
   - **Target:** under 0.1 ms per simple room, on the existing `zenith_floor_benchmark_run`.
   - **Files:** `src/engine/scene/PrefabSystem.cpp`, `Prefab.cpp`, `EntityCommandBuffer.*`, `Scene.*`.
3. **Fault isolation (E-4).**
   - **Guards:**
     - guard `SystemScheduler::run`, `Timers::advance`, command-buffer callbacks and `Effects`;
     - a throwing system is disabled for the session and reported, like `BehaviourScheduler::callHook` (`BehaviourScheduler.cpp:90-117`).
   - **Editor play loop:** catch around the play loop, then Stop Play with a diagnostic.
   - **Windows crash handling:** `SetUnhandledExceptionFilter` → minidump, plus an emergency scene autosave through the existing recovery path, wired to `DiagnosticBundleWriter`.
   - **Files:** `SystemScheduler.cpp`, `Timers.*`, `EntityCommandBuffer.cpp`, `src/editor/application/EditorPlayback.cpp`, `src/editor/main.cpp`, `src/main.cpp`.
4. **Graceful runtime limits (E-3).**
   - **Renderables over the cap:** draw the first N in a stable order, log once, and add a `RenderStats` counter instead of failing the frame.
   - **Physics step errors:** disable the offending body and continue. Treat cache overflow as a warning and raise the limits from `PhysicsSettings`.
   - **Audio:** priority-based voice stealing.
   - **Particles:** a warning when the 64-emitter cap is exceeded.
   - **Files:** `MeshPassRecord.cpp`, `Engine.cpp`, `PhysicsWorld.cpp`, `AudioSystem.cpp`, `Particles.*`.
5. **Transform API decision (E-5).**
   - **Helpers:**
     - `Scene::setLocalTransform`, `setPosition`, `setRotation`, `setScale` and `worldTransform(e)`, which mark dirty and lazily resolve world;
     - a TRS view (position, rotation, scale) over `local`.
   - **Audit:** make Debug's audit report rather than repair, so Debug and Release behave the same.
   - **Docs and template:** document that raw `registry()` writes to transforms are unsupported, and update the template and tutorial 03 to use the helpers.
   - **Navigation:** bake updates world transforms first.
   - **Files:** `src/engine/scene/Scene.hpp/.cpp`, `NavigationWorld.cpp`, `templates/fps_game/src/*`, `docs/tutorials/fps-game/03-*`.
6. **Generation preview foundations** (prerequisite for Part 3 #1).
   - **Transient entities:** add a `transient`/generated entity flag that `SceneDocument::serialize`, undo capture and Play-entry snapshots skip.
   - **Module operators:**
     - accept `OperatorProperties`, so a seed can be adjusted in Adjust Last Operation;
     - receive an Edit-mode service set: `RandomStreams`, a `PrefabSystem` that knows module components, and `DebugDraw`.
   - **Debug draw:** persistent and timed draws plus named channels, and labels in the game view.
   - **Files:**
     - `src/engine/scene/SceneDocument.cpp`, `Scene.hpp`;
     - `src/editor/modules/EditorModuleApi.hpp`, `GameModuleHost.cpp` (`:62`, `:428`, `:494`);
     - `src/engine/graphics/DebugDraw.*`, `src/game/application/*`.
7. **Small correctness fixes, best done together:**
   - particle emitter churn (E-6): keep per-emitter state by scene id instead of resetting all;
   - runtime asset collection on floor teardown and scene change (E-7);
   - sorted physics and character events (E-8);
   - the double nav bake;
   - a `GameServices` session resource store plus an asset-root path helper (2.2, last bullet);
   - updating FPS projects generated before E7 to `setPaused` and `zenith_configure_game_module`. Their `FpsGame.cpp` still pauses by clearing action fields, so physics, timers and animation keep running behind the menu.

### Critical follow-ups
Gaps the critical work left on purpose:
- **Graceful limits:** the GPU-driven draw-group limit (64 pipeline and geometry-page pairs) is still fatal. `MeshPass`'s older "reported once" flags could move to `OverflowEpisode`.
- **Fault isolation:** the crash handler does nothing outside Windows. `std::terminate` paths bypass it. A behaviour hook that throws is disabled but doesn't stop Play.
- **Runtime content:**
  - A scene that holds a runtime mesh can't be saved, so regenerate floors from their seed.
  - Material-override overflow is logged but not counted in `RenderStats`.
- **Spawning:** the prefab cache trusts an entry for 500 ms (`PrefabSystem::setFileCheckInterval`).
- **Transient previews:** a paste or a module reload drops the preview. Debug-draw channel visibility is global and isn't saved. `DemoApplication` draws no debug labels.
- **Asset collection:** runtime collection skips decoded audio held by emitters, collecting it only down to the audio budget.

### HIGH-VALUE EDITOR IMPROVEMENTS
1. **Prefab edit mode.** *Done, since superseded by prefab windows (`docs/editor/prefab-windows.md`):* prefabs open as their own documents over a parked scene (formerly `application/DocumentStack`), with their own undo, undoable saves and Apply, and instance refreshes on return, Apply, and outside changes; connector gizmos remain with Part 3 #2. Open a `.prefab` as its own document, with its own undo, and save with Apply. Also:
   - Apply becomes undoable through a file-backup command;
   - Apply refreshes every scene that uses the prefab;
   - connector gizmos (Part 3 #2).

   Do this before authoring room content (`src/editor/document/PrefabAuthoring.*`, a new document context, not more `EditorSession` members).
2. **Asset fields and materials.** *Done:* asset fields use a picker and drop target (`interaction/AssetReferenceField`), models are replaced with `object.model_replace`, and `.zmat` material assets are applied through `MaterialOverrideComponent::material_asset`. The original list:
   - an asset picker popup plus drop targets on every asset-typed reflected field, replacing `drawReadOnlyText` (`ReflectedFieldEditor.cpp:476`);
   - make Mesh Renderer's mesh assignable;
   - a material asset (`.zmat`: a glTF material override or a new PBR material) with an inspector, backed by Critical #1.
3. **Undo/duplicate/paste cost.** *Done:* operators declare an `OperatorScope` and the runner captures only that; Play captures nothing for history; copy and paste work on the selected subtrees and create copies in place (a group duplicate on a 5,000-entity scene went from 585 ms to 3 ms). Some prefab, collection and module operators still capture the whole scene. The original list:
   - operators declare the entities they touch and use `captureEntities`;
   - `SceneFragment` copies and pastes only the selection, with no `replaceWith` (`SceneFragment.cpp:72-76,160-206`, `OperatorRunner.cpp:76-99`);
   - skip capture in Play.
4. **Placement.** *Done:* the gizmo moves whole selections, drops land under the mouse with align-to-normal, the Add menu is complete, mesh-less objects can be picked, and there are select hierarchy/grouped, array and scatter operators. Not built: the drag ghost. The original list:
   - multi-selection gizmo (`ViewportPanel.cpp:1052`);
   - drop under the mouse with surface snap and align-to-normal;
   - full Add menu (point, spot, camera, audio emitter, prefab);
   - select children and select similar;
   - an array/scatter operator.
5. **Collision and navigation.** *Done:* colliders switch shape and take an offset, with resize handles; navmesh settings live in Project Settings and the game uses them too; Play overlays show physics shapes, contacts, characters, nav agents and the game's navmesh. The original list:
   - collider shape-type switch, offset, and drag handles;
   - nav bake settings UI and menu entries;
   - overlay toggles (physics, nav, crowd, contacts) that also work in Play, through `PhysicsWorld::drawDebug` and `NavigationWorld::drawDebug`.
6. **Animator inspector.** Edit states, transitions, parameters and events through reflected lists, not a node graph, with a "preview state" button (`AnimationComponentTypes.cpp:216`).
7. **Outliner scale and search.** *Done:* cached, clipped rows (an unchanged frame takes about 1.2 ms with 10,000 entities), `c:`/`t:`/`p:` filters with Select All Matching, saved sibling order with drag reordering, and collection rename. The original list:
   - `ImGuiListClipper` and incremental rebuild (`HierarchyPanel.cpp:164`);
   - filter by component and tag;
   - manual sibling order;
   - collection rename.
8. **Assets.** *Done:* import by OS drop and dialog, texture import settings, texture and audio reload, Find References with a delete guard, and definition tables as data assets with module-registered schemas; thumbnails were done with item 2, and the mesh row with `object.mesh_assign`. The original list:
   - OS drag-and-drop import;
   - import-settings panel;
   - texture and audio reload;
   - drawn thumbnails;
   - find references and a delete guard;
   - data (`.json` definition table) as an asset kind, with a table view and validation errors in the Info log.

### BUILD WHEN NEEDED
These are the triggers in 2.2, each to be built when the game first asks for it:
- tiled or async nav, obstacles, off-mesh links, area costs;
- blend trees, layers, root motion;
- compound or convex shapes, joints, shape cache, teleport;
- audio rolloff curves, reverb, buses;
- GameUi wrap, scroll, 9-slice;
- input contexts;
- gameplay job API;
- immediate signals and coroutines;
- spatial index;
- scalar lists in reflection;
- Tracy-style markers, file log sink, console;
- reflection-probe baking UI (`ReflectionProbeBaker` exists but is unwired);
- async irradiance bake;
- shader hot reload (only if writing custom shaders);
- audio preview in the editor.

### ENGINE DIFFERENTIATORS
In suggested order, each depending on the Critical tier:
1. Connector-based kit assembly (Part 3 #2). It needs Critical #2 and pairs with prefab edit mode.
2. Staged, inspectable generation pipeline plus the Generation panel (Part 3 #1). It needs Critical #6.
3. Seed farms via `GameplayRunner` (Part 3 #3), so CI asserts generation invariants.
4. Deterministic input replay (Part 3 #4). It needs Critical #7's event sorting.
5. Promote generated → authored (Part 3 #5).
6. Spatial fields and influence maps (Part 3 #6), ahead of the perception layer.
7. Procedural greybox kit (Part 3 #7). It needs Critical #1.

### FUTURE WORK
Impressive, but heavy on complexity now:
- **Time-travel debugging:** a per-tick scene snapshot ring with rewind in the editor.
- **Wave-function-collapse or constraint solving:** with propagation visualised step by step.
- **A node-graph editor for generation pipelines**, built only after the code-defined stage API has proven its shape.
- **Live++-style patching during Play.** Reload currently stops Play first (`GameModuleHost.cpp:357`).
- **Navmesh tile rebuilds for the mutable "living dungeon" world.**
- **SDF or CSG level geometry and destruction.**
- **Multi-scene editing.**

---

## Verification
- Every cited file and line was read or spot-checked during the review, including:
  - the `Assets.hpp` private creators;
  - the `PrefabSystem` double build;
  - the single-tile `NavigationWorld`;
  - unguarded `SystemScheduler::run`;
  - the `MeshPassRecord` full rebuild and 16,384 cap;
  - `Engine.cpp` fatal physics and draw paths;
  - `Particles.hpp` cap and reset;
  - the release-only transform audit;
  - `collectUnused` callers;
  - the single-selection gizmo;
  - whole-scene capture in `OperatorRunner`;
  - Play-entry serialization;
  - read-only asset fields;
  - no replay and no transient flag.
- Each Critical item's own plan carries its tests (for example, `zenith_floor_benchmark_run` for #2, a throwing-system test for #3, and a cap-exceeding scene for #4).
