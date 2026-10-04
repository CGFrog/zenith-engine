# Gameplay roadmap

This roadmap makes a typical first-person game programmable end to end from
the editor, as a hot-reloaded C++ game module. It follows the completed editor
roadmap (reflection, module hot reload, operators, workspaces, prefabs, play
targets). Each item is sized so that one session, or one `/batch` unit, can
finish it.

Approved 2026-10-04. Phases A to D are done. Phase E, the foundations for a
first-person roguelike built as a separate project, was approved the same day
and is open.

## How to use this document

- **Before starting.** Read [Status](#status) and pick an unchecked item whose
  dependencies are all checked. Read the "Read first" docs listed for its
  phase.
- **While working.** Follow `AGENTS.md`, the nearest nested `AGENTS.md`, and the
  [Conventions for every item](#conventions-for-every-item) below.
- **When finishing.**
  - Tick the item's boxes and set its row in the status table to `done`, with
    the commit hash.
  - Note any follow-ups under the item, or under
    [Open questions and follow-ups](#open-questions-and-follow-ups).
  - Move feature details into the subsystem doc (`docs/engine/*.md`,
    `docs/editor/*.md`). This file tracks only scope and status.
- **When the plan changes.** Edit the item's text and record the decision in
  [Decisions](#decisions) with a date, so later sessions do not reopen it.

## Decisions

- **2026-10-04: C++ Behaviours on top of the ECS.** Per-entity logic uses a
  C++ `Behaviour` base class whose concrete types are reflected component
  types. They are hot reloaded in the game module DLL. Plain systems remain the
  tool for bulk logic. Python and other scripting languages were rejected for
  performance and are not to be re-proposed. This settles the "choose a
  scripting runtime" item of `NEXT_STEPS.md` Milestone 20.
- **2026-10-04: `Scene` maintains its id index itself (A1).** The roadmap
  suggested EnTT construct/destroy signals on `SceneEntityIdComponent`.
  `Scene` is the only code that creates or destroys entities or writes that
  component, so it updates the index directly. Signal handlers bind the
  `Scene` instance, and `swap`/`replaceWith` move the registry to another
  `Scene`, so signals would have updated the wrong index.
- **2026-10-04: `GameServices` ownership and names (A2).**
  - `GameServices` is owned by what runs gameplay:
    - `GameplayRunner`, used by a game's `ApplicationScene`;
    - the editor's `GameModuleHost`, for each Play or Simulate session.
  - Systems reach it through `SystemContext::services`, not through
    `ApplicationSession`/`ApplicationContext`: its `PrefabSystem` needs the
    gameplay's component registry, which only those owners know.
  - It is null in Edit mode.
  - The deferred command list is `EntityCommandBuffer`, because the editor
    already has a global `EntityCommands` namespace.
  - Event types are keyed by a `static constexpr eventKey` string. A C++ type
    id would differ between the engine and a module DLL.
- **2026-10-04: physics layers and triggers (A3).**
  - `PhysicsLayer` is a one-index struct with the built-in `Static`, `Dynamic`
    and `Player` constants, so existing code keeps compiling. Reflection
    stores it as `UInt32`.
  - Each Jolt object layer is the index times two plus a moving bit, so one
    named layer holds static and moving bodies.
  - Physics publishes nothing itself: `PhysicsWorld::triggerEvents()` lists
    each step's events and the host forwards them to the `EventQueue`.
  - A body that falls asleep inside a sensor stays inside. Jolt drops a
    sleeping body's contacts, which would otherwise look like an exit.
- **2026-10-04: the engine character controller (B1).**
  - The engine's `CharacterControllerComponent` took over the demo's
    `character_controller` key at schema version 2. The version 1 migration
    drops the look settings and adds the new fields at their defaults.
  - A character is not a rigid body: an entity with the component gets a Jolt
    `CharacterVirtual` and no body. Its kinematic inner body carries the
    entity, so queries, sensors and contacts see it.
  - The view's yaw and pitch live on the camera child's
    `FirstPersonCameraComponent`; the character's own rotation never changes.
    The camera keeps the eye height it was placed at.
  - The components name the input actions that drive them (`Move`, `Look`,
    `Jump`, `Sprint` by default). An empty move action leaves a character to
    code, so engine code never hard-codes a game's action map.
  - The engine's own systems (`registerBuiltInSystems`) seed every
    `SystemRegistry`, so they run in games and in editor Play alike, before a
    game's systems of the same phase and order. Later items add theirs there.
  - `ZENITH_MODULE_API_VERSION` is 5.
- **2026-10-04: how behaviours run (B2).**
  - `Behaviour` lives in `src/engine/behaviour/`, with its registry and
    scheduler.
  - Behaviours run inside three engine systems (`zenith.behaviours`,
    `zenith.fixed_behaviours`, `zenith.late_behaviours`), so hosts needed no
    new phase hooks, flushes follow the existing rules, and the Simulate mask
    applies to them as to any system.
  - The behaviour types reach the systems through `SystemContext::behaviours`
    and `GameServices`, not through the component registry, so `scene/`
    knows nothing of behaviours.
  - Trigger and collision hooks run at the start of the next FixedUpdate,
    from events `GameServices::publishPhysicsEvents` keeps by scene id.
  - `onDestroy` runs only for destroy commands; there is no ECS signal
    (see the A1 decision on signals and `Scene::swap`).
  - A copied behaviour starts uncreated; assignment keeps lifecycle state.
  - `ZENITH_MODULE_API_VERSION` is 6.
- **2026-10-04: navigation (B7).**
  - Recast/Detour (recastnavigation v1.6.0) is fetched like bc7enc_rdo: only
    its sources, as the private `zenith_recast` library. A worktree needs the
    main build configured once after this change, so `recastnavigation-src`
    exists in the shared `_deps`.
  - One solo tile per scene. A game bakes at runtime on first use
    (`GameServices::navigation`); baked meshes are not saved yet.
  - The editor preview is editor state (`navigation.bake`), not part of the
    document, and every viewport draws it while it exists; there is no
    overlay toggle, because viewport overlays are persisted settings.
- **2026-10-04: code generator snippets (C2).**
  - The snippets live in `templates/cpp_game/snippets/` as `*.in` files, and
    the Project Hub skips that folder when it copies a template, so their
    tokens never reach a project. The editor reads them from the template it
    found at startup, so every project generates from the editor's version.
  - Generated components and systems get a `Component` or `System` suffix;
    behaviours keep their name.
  - Generating builds through `build.run`'s queue, so a reload follows.
- **2026-10-04: project settings (C5).**
  - Tags are a project tag list (manifest `tags`) plus the engine's
    `TagsComponent` of names; any name works at runtime, and the editor
    marks names the project does not declare.
  - A project's default input actions live in a JSON file the manifest names
    (`inputActions`, version 6). Its actions replace the module's of the same
    name whole, so code-declared actions keep working without the file.
    Sources are written as names (`key:W`), not enum numbers.
  - Physics layers are added and removed only at the end of the table.
- **2026-10-04: FPS template (C6).**
  - Behaviours get `BehaviourContext::physics` for queries, so the hitscan
    weapon stays a behaviour (module API version 7).
  - The template's scene and prefab are generated by the engine's serializers
    (`FpsTemplateTests --write`) and committed, not built when a project is
    created; module components in them are kept as JSON until the module
    loads.
  - The template's input actions come from its project file only; its module
    registers none in code.
- **2026-10-04: Phase D scope.**
  - D7 restores `PERFORMANCE_ROADMAP.md` rather than removing its links: it
    is the record of the renderer's measurement contract and baselines.
  - D5 adopts the helper in the files that really are versioned JSON. The
    roadmap's ViewportChrome and HierarchyPanel store unversioned area state
    inside `workspaces.json`, and the editor preferences are `key=value`
    text, so they are left as they are.
  - D6 also measures the runtime spawn path (`PrefabSystem::instantiate`
    from `EntityCommandBuffer` and effects), not only the editor's compare
    and refresh.
- **2026-10-04: Phase E scope (roguelike foundations).**
  - The next game is a first-person roguelike. It is built first as a
    generic roguelike: generated floors, combat against path-finding enemies,
    loot, stats, and permadeath with some meta-progression.
  - Its perception model (sight, sound and disturbance signals feeding enemy
    beliefs) and its mutable world are deferred. So is the engine work they
    need: climb, swim, fly and wall-walk movement modes, stimulus events, a
    CPU light-level query, through-walls overlays, water and buoyancy,
    breakable constraints, and tiled navmesh rebuilds.
  - A run is saved as its seed, floor and run state, and the floor is
    regenerated on load. A scene diff (B5) of a fully spawned floor would be
    nearly the whole scene, and would tie saves to the generator's output.
  - Floors are spawned into the main scene, not as additive scenes: physics
    and navigation each work on one `Scene`.
  - The game is a project created from `templates/fps_game`, not code in
    `src/game/`. Phase E adds only reusable mechanisms; the generator, AI
    states, stats, items and loot are the game's.
- **2026-10-04: definition tables (E3).**
  - Reflection gained `FieldKind::List` (a `std::vector` of nested structs)
    rather than tables flattening lists into one row per element: a loot
    table's drops, an enemy's attacks, and a floor theme's rooms are lists.
  - Tables reject unknown keys, while scene documents keep ignoring them.
    Tables are written by hand, so a typo must fail. Scene documents must
    stay readable by older builds.
  - A table reloads by comparing its file's write time and size, not
    through the editor's file watcher, so the mechanism is the same in a
    shipped game and needs no editor wiring.
- **2026-10-04: engine owns mechanisms, games own rules.** The character
  controller, physics queries, events, timers, effects, save slots and UI
  widgets are reusable engine mechanisms. Weapons, damage, HUD contents and
  game flow stay in game modules and templates.

## Status

| Item | Title | Depends on | Status |
| --- | --- | --- | --- |
| A1 | Scene costs that grow with entity count | — | done d1519ef |
| A2 | System phases and `GameServices` | — | done 7f4f340 |
| A3 | Physics queries, layers, triggers | A2 (trigger events) | done 71dfd12 |
| A4 | Shared input actions | A2 | done 618c2ad |
| B1 | Engine character controller | A3, A2 | done ce2cb6f |
| B2 | Behaviours | A2, A3 | done ebd2b43 |
| B3 | Animation state machine | A2 | done a0c37f2 |
| B4 | Effects and pooling | A2 | done e4f9ef2 |
| B5 | Save game | A2 | done f98cdbb |
| B6 | Game UI widgets and settings screen | A2 | done c8ed439 |
| B7 | Navigation | A3 | done 106f547 |
| B8 | Audio polish | A3 | done 41ac78d |
| C1 | Build as an operator | — | done 84c11ae |
| C2 | Code generators | B2, C1 | done a1aa3f7 |
| C3 | Inspector integration for Behaviours | B2 | done 100cc6d |
| C4 | Debugging tools | A2 | done e68d709 |
| C5 | Project settings editors | A3, A4 | done 8975b5d |
| C6 | FPS template and tutorial | B1–B6, C2, C3 | done 6a257db |
| D1 | Split `PrefabSystem.cpp` | — | done 10eb690 |
| D2 | Split `EditorSession.cpp` | — | done 122b2bb |
| D3 | Split `FileWatcher.cpp` per backend | — | done fe541bd |
| D4 | Targeted diffs in `SceneEditing` | — | done a8e58bd |
| D5 | Shared versioned-settings helper | — | done 89d1a71 |
| D6 | Measure prefab compare/refresh cost | — | done d518873 |
| D7 | Docs hygiene | — | done c5b111b |
| E1 | Seeded gameplay random streams | — | done ba6064d |
| E2 | Run and profile save data | E1 | done |
| E3 | Definition tables | — | done 4174590 |
| E4 | Building and replacing a floor at runtime | — | open |
| E5 | Partial paths and crowd avoidance | E4 | open |
| E6 | Audio for gameplay | — | done 4bb4465 |
| E7 | Pause, teardown and one game clock | — | done b26c8ab |

Suggested batches:

1. **A1–A4, C1, D7.** Small and independent; they remove every hard blocker.
2. **B1, B2, C2, C3.** They deliver the scripting model.
3. **B3–B6, C4–C6.** They deliver the FPS template.

B7, B8 and D1–D6 fit in anywhere. Measure D4 and D6 before changing code.

Phase E batches:

1. **E7, E1, E3, E6.** Small and independent. Do E7 first: it changes
   behaviour the FPS template relies on, while that template is the only
   game to update.
2. **E2, E4, E5.** E4 measures before it changes anything; E5 builds on its
   rebake.

```text
A1 ─┐
A2 ─┼─> B1 ─┐
A3 ─┤       ├─> B2 (Behaviours) ─> C2, C3 ─> C6 (FPS template + tutorial)
A4 ─┘       │
            └─> B3, B4, B5, B6 (parallel)          B7, B8 later
C1, C4, C5 can start after A2/A3/A4; D1–D7 anytime
```

## Why: the gaps as of 2026-10-04

- **Hard blockers for a module-only game.**
  - No HUD: systems have only `Update` and `FixedUpdate`
    (`src/engine/module/SystemScheduler.hpp`), and `ProjectApplication` never
    overrides `ApplicationScene::drawUi`.
  - `SystemContext` carries only the scene, an optional `ApplicationContext`,
    the mode, the delta and raw `InputState`. There is no prefab spawning,
    deferred destroy, events, timers or input actions.
- **Missing mechanisms.**
  - Physics: `PhysicsWorld::raycast` takes no filter, so a ray from the camera
    hits the player's own capsule. There are no shape casts, overlaps or
    trigger enter/exit events, and `PhysicsLayer` is a fixed three-value enum.
  - The character controller is demo code
    (`src/game/player/FirstPersonController.*`) that decides "grounded" by
    scanning contacts.
  - None of these exist yet: animation state machine, effects API, save game,
    navigation, UI widgets beyond panel/text/image/button.
- **No per-entity scripting model.** `NEXT_STEPS.md` Milestone 20 is unchecked.
- **Editor loop gaps.** No Build operator or shortcut, no code generators, no
  jump from the inspector to the source, no debugger attach.
- **Stale tutorial.** `docs/tutorials/fps-game/` teaches a separate `src/fps/`
  executable, raw ImGui and hard-coded keys instead of the module workflow.

## Phase A: engine prerequisites

Read first: [Scene](engine/scene.md), [Game modules](engine/modules.md),
[Application](engine/application.md), [Physics](engine/physics.md),
[Window and input](engine/window-input.md).

### A1. Scene costs that grow with entity count

Files: `src/engine/scene/Scene.{hpp,cpp}`, `src/engine/physics/PhysicsWorld.cpp`,
`src/engine/Engine.cpp`.

- [x] Stop `Scene::markTransformDirty` from searching `m_dirtyTransformRoots`
      on every call. Today physics calls it twice per active body per frame,
      so the cost grows with the square of the body count. Use the
      `TransformCacheComponent::dirty` flag as the membership test, or a hash
      set.
- [x] Remove the linear finds in the `updateWorldTransforms` root walk the
      same way.
- [x] Stop `updateBranch` from copying each node's child vector. Keep it
      correct if the hierarchy changes during iteration.
- [x] Make `Scene::findEntity(SceneEntityId)` constant time. It currently scans
      the whole scene and is called about 100 times across 36 files. Keep an
      id-to-entity map updated from EnTT construct and destroy signals on
      `SceneEntityIdComponent`, so there is one place to maintain.
- [x] `PhysicsWorld::interpolate` and `Engine.cpp` both call
      `updateWorldTransforms` each frame. Remove one call, or prove the second
      is a no-op.
- [x] Tests: id lookup after create, destroy, duplicate and document load; many
      dirty bodies; hierarchy changes during an update.

Exit criteria: transform update stats and frame timings are recorded before
and after, in a scene with about 5k dynamic bodies, and the comparison is
noted here.

Result: `tests/engine/SceneScalingBenchmark.cpp`, which is
`zenith_scene_scaling_benchmark` (Release, 5,000 awake dynamic boxes, 60
measured frames of step, then interpolate, then `updateWorldTransforms`, plus
10k `findEntity` lookups). Mean milliseconds:

| Measure | Before | After |
| --- | --- | --- |
| `physics.step` | 5.08 | 1.43 |
| of which write-back (`sync_out`) | 4.08 | 0.69 |
| `physics.interpolate` | 7.20 | 0.49 |
| `updateWorldTransforms` | 3.55 | 0.48 |
| Frame total (step + interpolate + transforms) | 15.83 | 2.40 |
| 10k `findEntity` lookups | 22.48 | 0.17 |
| Creating the 5,001 entities | 12.98 | 2.07 |

Transform stats are unchanged: 5,000 visited and 5,000 recomputed per update.

- The second `updateWorldTransforms` call is gone. `interpolate` updates the
  scene first only when an interpolated body has a parent; the frame's own
  call then recomputes only the interpolated bodies.

### A2. System phases and `GameServices`

Files: `src/engine/module/SystemScheduler.hpp`, `ModuleRegistrar.hpp`,
`src/engine/application/Application.hpp`, `src/engine/Engine.cpp`,
`src/game/application/ProjectApplication.*`, editor play mode
(`src/editor/application/EditorPlayMode.*`).

- [x] Add `SystemPhase::LateUpdate`, which runs after physics interpolation
      (for cameras).
- [x] Add `SystemPhase::Ui`, which runs inside the engine's UI frame. Hook it
      into `ProjectApplication::drawUi` and the editor's play mode.
- [x] Add `GameServices`, owned by the application session and reached
      through `ApplicationContext`, with a pointer in `SystemContext`:
  - [x] `PrefabSystem&`, built with the session's component-type registry.
  - [x] `EntityCommands`: deferred spawn, destroy and reparent, flushed at a
        documented point after each phase.
  - [x] `EventQueue`: typed and double-buffered per frame. Engine events are
        `TriggerEntered`, `TriggerExited`, `EntitySpawned`, `EntityDestroyed`
        and `SceneLoaded`. Modules can declare their own event types.
  - [x] `Timers`: one-shot or repeating, optionally owned by an entity, scaled
        by the simulation clock.
  - [x] `DebugDraw&`, the existing engine debug draw.
- [x] Give `Logger` to systems in every mode, including Edit, where
      `application` stays null.
- [x] Tests: phase order, Ui systems run in the stock runtime, flush semantics
      of `EntityCommands`, event lifetime across frames, timers under time
      scale and pause.

Exit criteria: a test game module draws a HUD element through a Ui system, in
both `zenith --project` and editor Play.

Result:
- `tests/engine/modules/TestGameModule.cpp` registers a `hud` Ui system.
  `ProjectRuntimeTests` plays it through `ProjectApplication` and finds its
  text in the `GameUiContext` after `drawUi`.
- `tests/editor/modules/EditorTestModule.cpp` does the same. In
  `GameModuleHostTests`, its Ui phase draws into a game-view `GameUiContext`.
  `EditorSession::drawGameViewUi` runs it each Play frame, and `GameUiOverlay`
  draws it over the game view.

Follow-ups:
- The editor's game-view overlay draws text in the ImGui font and images as
  their tint. A per-view GameUi pass in the renderer would remove the
  difference.
- The System Profiler counts Ui system time in the following frame, because
  the profile frame ends at the start of `drawUi`.

### A3. Physics queries, layers and triggers

Files: `src/engine/physics/*`, `src/engine/scene/PhysicsComponentTypes.cpp`,
`src/engine/project/` (manifest).

- [x] Add a `PhysicsQueryFilter` with a layer mask, ignored entities and a
      hit-sensors flag, accepted by `raycast`.
- [x] Add `raycastAll`, `shapeCast` (sphere, capsule, box), `overlap` and
      `closestPoint`.
- [x] Add named project layers: up to 32, declared in the project manifest,
      with a collision matrix. `Static`, `Dynamic` and `Player` stay as the
      built-in defaults. Changing the serialized field from an enum to an
      index needs a component schema version and a migration.
- [x] Derive per-sensor trigger enter and exit events from `contactEvents()`
      and publish them to the A2 `EventQueue`.
- [x] Tests: a ray that ignores its owner, layer masks, shape casts, overlap
      results, and the enter/stay/exit order for triggers.

Result:
- `PhysicsWorld` has `raycast` and `raycastAll`, `shapeCast`, `overlap`, and
  `closestPoint`, all with a `PhysicsQueryFilter`.
- Layers come from `PhysicsLayerTable`, which project manifest version 5
  stores as `physicsLayers`. The game and the editor apply it through
  `setLayers`.
- The `rigid_body` component is at schema version 2; its migration from
  version 1 is a no-op.
- Coverage is in `tests/engine/PhysicsQueryTests.cpp` and
  `ProjectConfigTests`.

Follow-ups:
- The inspector edits the layer as a number until C5 adds the layer editor.
  It needs project-supplied names in `ReflectedFieldEditorOptions`.
- Sensor-vs-sensor overlaps are not reported.
- The stale first-person controller sentence in `docs/engine/physics.md` was
  rewritten with the layer change; B1 and D7 need not fix it again.

### A4. Shared input actions

Files: `src/engine/input/*`, `src/engine/module/ModuleRegistrar.hpp`.

- [x] Modules declare their action map with `registrar.addInputActions(...)`.
- [x] The host evaluates the map once per frame and exposes a
      `const InputActionState*` in `SystemContext`. Systems no longer evaluate
      bindings themselves (as `src/game/gameplay/DemoGameplay.cpp` does today).
- [x] `InputProfileSet` rebinding and save/load keep working.
- [x] Port the demo gameplay to the shared state.

Result:
- `ModuleRegistrar::addInputActions` merges a module's actions into one
  `InputActionMap`, which `GameModule`, `Gameplay`, and the editor's
  `GameModuleHost` carry.
- `GameplayRunner` evaluates the map once per frame and once per fixed step,
  and hands systems `SystemContext::actions`.
- `applyInputProfile` overlays a saved `InputProfile` on the runner's
  `actionMap()`. `InputProfileSet` and its save and load are unchanged.
- The demo registers `createDemoInputMap()`. Its systems and
  `GameplayScene`'s menus read the same map.

Follow-ups:
- Nothing persists a player's profile per game yet. The settings screen (B6)
  and C5 should save one beside the game settings and apply it at start.
- `ButtonBehavior::Toggle` is still not evaluated.

## Phase B: reusable gameplay framework

Read first: Phase A docs plus [Assets](engine/assets.md) and [UI](engine/ui.md).

### B1. Engine character controller

- [x] Add `CharacterControllerComponent` backed by Jolt `CharacterVirtual`,
      with Jolt kept private. It has a slope limit, step height, a ground probe
      (an A3 shape cast), ground normal and velocity, and moving-platform
      support.
- [x] Add `FirstPersonCameraComponent` (pitch clamp, sensitivity) and a
      LateUpdate camera system.
- [x] Move the generic parts of `src/game/player/FirstPersonController.*` and
      the player bootstrap in `src/game/scene/GameplayScene.cpp` into the
      engine. Remove the duplicate `firstCameraChild`/`resolveCamera`.
- [x] Port the demo. Fix the stale controller line in `docs/engine/physics.md`.
- [x] Tests: walking up steps, the slope limit, staying grounded on a moving
      platform, jumping.

Result:
- `src/engine/physics/CharacterController.hpp` and `PhysicsWorld` step the
  characters before the bodies; `src/engine/gameplay/FirstPerson.*` holds the
  camera component, `applyCharacterInput`, `updateFirstPersonCameras`,
  `spawnFirstPersonPlayer` and `findCameraChild`.
- `src/engine/module/BuiltInSystems.*` registers `zenith.character_input`
  (FixedUpdate) and `zenith.first_person_camera` (LateUpdate).
- The demo lost `src/game/player/`; its scenes were re-saved with the engine
  components. The physics doc had no stale line left; `docs/game/demo.md` and
  `ARCHITECTURE.md` were fixed instead.
- Coverage is `tests/engine/CharacterControllerTests.cpp`
  (`zenith_character_controller`), plus the demo's module and lifecycle tests.

Follow-ups:
- The editor draws no gizmo for a character's capsule.
- Characters do not push each other (Jolt's character-vs-character
  collision is not set up), and a character leaving a moving platform loses
  the platform's velocity.
- `docs/tutorials/fps-game/` still teaches the removed demo controller; C6
  rewrites it.

### B2. Behaviours

Design:
- `Behaviour` is an engine base class (`src/engine/behaviour/`, or
  `src/engine/module/` if it stays small).
- Hooks: `onCreate`, `onStart` (once, on the first frame alive), `onUpdate`,
  `onFixedUpdate`, `onLateUpdate`, `onTriggerEnter`, `onTriggerExit`,
  `onCollision`, `onEnable`, `onDisable`, `onDestroy`.
- Each hook receives a `BehaviourContext`: entity, scene, `GameServices`,
  actions, delta.

Tasks:
- [x] Each concrete behaviour is a reflected component type, registered with
      `registrar.addBehaviour<T>(key, displayName, Fields<T>()...)`. That gives
      the inspector, serialization, multi-edit, undo, prefab overrides and
      `PreservedComponentsComponent` for free.
- [x] The registrar stores type-erased dispatch thunks per behaviour type, and
      records the declaration's `std::source_location`.
- [x] Add a `BehaviourScheduler` that runs behaviours in a deterministic order:
      phase, then behaviour registration order, then entity creation order.
      Behaviours run in Play and Simulate. A type can opt into Edit with the
      same `RunIn` flags systems use.
- [x] Destroying or spawning from inside a hook goes through `EntityCommands`.
- [x] Reload survival uses the existing serialize/restore path.
- [x] Tests: hook order, `onStart` runs exactly once, destroy during
      iteration, trigger hooks, survival across hot reload (in the style of
      `zenith_game_module_host_tests`).
- [x] Document when to use a Behaviour and when to use a system in
      `docs/engine/modules.md`. Update `NEXT_STEPS.md` Milestone 20.

Result:
- `src/engine/behaviour/`: `Behaviour.hpp` (hooks, `BehaviourContext`),
  `BehaviourRegistry.*`, and `BehaviourScheduler.*` (`runBehaviours`,
  `destroyBehaviours`). `ModuleRegistrar::addBehaviour<T>` registers both
  halves; `Gameplay` and `GameModule` carry the registry.
- Coverage: `tests/engine/BehaviourTests.cpp` (`zenith_behaviours`), and a
  behaviour in `EditorTestModule.cpp` whose fields survive the reload in
  `GameModuleHostTests`.

Follow-ups:
- A hook that throws is not isolated (NEXT_STEPS Milestone 20).
- Stopping a session or unloading a scene calls no `onDisable`/`onDestroy`.
- Update systems in a game host get the unscaled frame time while timers and
  animations use simulated time; behaviours inherit that.

### B3. Animation state machine

- [x] Add `AnimatorComponent` with parameters (float, bool, trigger), states
      bound to clips, and transitions with conditions and cross-fade time. It
      is built on `beginAnimationCrossFade` and `blendAnimationPoses`.
- [x] Animation events at clip times are published to the `EventQueue`.
- [x] The first version is defined in JSON and code. A graph editor is later
      work.

Result:
- `src/engine/animation/Animator.*`: `AnimatorDefinition`, `updateAnimator`
  (one animator against a clip list, testable without assets) and
  `updateAnimators`. The `animator` component type is in
  `AnimationComponentTypes.cpp`; `zenith.animator` runs it in Update after the
  game's systems, before `updateAnimations`. Events are `AnimationEvent`.
- Transitions can also wait for an exit time (clip lengths played), and
  `from` may be empty for an any-state transition.
- Coverage: `tests/engine/AnimatorTests.cpp` (`zenith_animator`); docs in
  `docs/engine/animation.md`.

Follow-ups:
- States address clips by name; a model whose clips are renamed leaves its
  animators inert (`updateAnimator` returns false) without a diagnostic.
- Both clips of a cross-fade share the target state's speed and looping,
  because `AnimationComponent` has one of each.
- The animator advances on the Update frame time, `updateAnimations` on
  simulated time; under a time scale their state times drift apart.

### B4. Effects and pooling

- [x] Add `effects.spawn(prefab, transform, lifetime)` to `GameServices`. The
      spawned entity is destroyed, or returned to a pool, when its particle
      burst ends or its lifetime expires.

Result:
- `src/engine/module/Effects.*`, reached as `GameServices::effects()`.
  Particles simulate on the GPU, so "the burst ends" is derived on the CPU:
  the longest `lifetimeMax` of the prefab's one-shot emitters
  (`derivedEffectLifetime`). Continuous or repeating emitters need an
  explicit lifetime; without one the spawn is logged and destroyed.
- Pooling parks the instance with its emitters off; reuse moves it and bumps
  the new runtime-only `ParticleEmitterComponent::restartGeneration`, which
  the particle scheduler treats as a restart of the first burst.
- Coverage: `testEffects` in `GameServicesTests.cpp` and
  `testSchedulerRestart` in `ParticleTests.cpp`.

Follow-ups:
- Parked instances stay visible: pooling suits particle-only effects until
  there is a way to hide an entity subtree from rendering.
- Pools are unbounded and live as long as the gameplay session.

### B5. Save game

- [x] Add a `SaveGame` API over `SceneDocument` serialize and diff, with slots
      under the platform's writable location.
- [x] Add a `.runtimeState()` field hint: fields marked with it are skipped in
      scene documents but kept in saves.

Result:
- `src/engine/scene/SaveGame.*` (`SaveGameStore`); `SceneDocumentOptions::
  purpose` selects whether runtime-state fields are written
  (`RuntimeStateFields` in `FieldJson`).
- A save stores only the changed entities' new states against the baseline
  scene document, so untouched entities follow later level edits.
- `GameServices::saveGames()`/`saveGame()`; `ProjectApplication` and the
  editor's Play set the store and the baseline.
- Coverage: `tests/engine/SaveGameTests.cpp` (`zenith_save_game`).

Follow-ups:
- Loading a save into a running session is the game's flow (a scene request
  that loads the slot); C6's template should show it.
- A baseline written in an older document version diffs against every
  entity, so such saves are larger until the level is re-saved.
- Undo snapshots also leave runtime state out; a Play-mode edit that is undone
  resets runtime fields to their defaults.

### B6. Game UI widgets and settings screen

- [x] Add slider, toggle, progress bar and simple row/column layout to
      `GameUiContext`.
- [x] Move the demo's pause and settings screens out of
      `src/game/scene/GameplayScene.cpp` into a reusable engine screen built on
      `GameUi`.

Result:
- `GameUiContext::toggle`, `slider` (pointer drag, and left/right
  navigation through the new `UiNavigationInput` fields), `progressBar`, and
  `UiLayout`; widgets share one focus and frame helper with `button`.
- `src/engine/ui/GameMenus.*`: `drawPauseMenu` and `drawSettingsMenu` over
  game-supplied `SettingsEntry` controls, so the engine screen holds no demo
  policy. The demo's settings gained a volume slider (`GameSettings`
  version 3) and a sensitivity slider.
- Coverage: `GameUiTests.cpp`, the new `GameMenusTests.cpp`
  (`zenith_game_menus`), and `GameSettingsTests.cpp`.

Follow-ups:
- Key rebinding is not on the settings screen: the input layer has no "any
  key pressed" query and only 14 keys. A4's follow-up to persist the input
  profile still stands.
- `ProjectApplication` has no pause or settings screen; C6's template should
  use the engine menus there.

### B7. Navigation

- [x] Add Recast/Detour behind a `NavigationWorld`, with navmesh baking from
      static colliders and a `NavAgentComponent` that follows paths.
- [x] Add an editor bake operator, with the navmesh shown through `DebugDraw`.

Result:
- `src/engine/navigation/`: `NavigationWorld` (bake, `findPath`,
  `nearestPoint`, `drawDebug`), `gatherNavigationGeometry`, and
  `NavAgentComponent` with `updateNavAgents`; the `nav_agent` type is in
  `scene/NavigationComponentTypes.cpp`, and `zenith.nav_agents` runs agents
  on the session's mesh.
- Editor: the `navigation` operator family (`navigation.bake`,
  `navigation.clear`) and the viewport drawing in `ViewportPanel`.
- Coverage: `tests/engine/NavigationTests.cpp` (`zenith_navigation`) and
  `OperatorFamilyTests`. Docs in `docs/engine/navigation.md`.

Follow-ups:
- Save baked meshes (with the scene or as an asset) instead of baking at
  runtime, and use tiles for large worlds.
- Agents do not avoid each other (DetourCrowd is not built).
- The bake operators are reachable through F3 only; no menu lists them.

### B8. Audio polish

- [x] One-shot sounds that follow an entity.
- [x] Optional occlusion using an A3 filtered raycast.

Result:
- `AudioSpatialOptions::follow` (with `position` as an offset) and
  `occlusion`, `AudioEmitterComponent::occlusion`, and
  `AudioSystem::setOcclusionQuery`. `syncScene` is split into following,
  emitters, and occlusion; occlusion queries run outside the mixer's lock.
- `Engine` answers occlusion with a raycast against the Static layer.
- Coverage: `testFollowingVoices` and `testOcclusion` in
  `AudioSystemTests.cpp`.

Follow-ups:
- The audio components still have no serializable component types, so
  scenes cannot author emitters, listeners, or their occlusion yet.
- Occlusion is all or nothing per ray and ignores project layers other than
  Static; partial occlusion (several rays, materials) is later work.

## Phase C: the script workflow in the editor

Target loop: create a script from the editor, edit it in the IDE, press
Ctrl+B, the editor hot reloads, the script shows in the inspector, then play
and debug.

Read first: [Game module hosting](editor/modules.md),
[Build and IDE workflow](editor/build-ide.md),
[Operators and keymaps](editor/operators.md),
[Inspector](editor/inspector.md), [Play mode](editor/play-mode.md),
[Editor extensions](EDITOR_EXTENSIONS.md), [Editor smoke](editor/smoke.md).

### C1. Build as an operator

- [x] Add `build.run` (Ctrl+B) and `build.cancel` operators. The Build panel
      button (`src/editor/panels/BuildPanel.cpp`) calls them.
- [x] Add an optional build-on-save preference. It uses `FileWatcher` on the
      module's source directory, debounced like `GameModuleHost`.
- [x] When a build finishes during Play, show "Reload pending" in the status
      bar. The reload already waits for Stop.

Result:
- `src/editor/project/ProjectBuild.*` owns the build the Build panel used to
  own: the request, the runner, and the `Build` diagnostics. The session polls
  it every frame, so Ctrl+B builds with no Build area open.
- `operators/BuildOperators.*` is the `build` family. `build.run` during a
  build queues one more build; `build.cancel` drops it.
- `build.on_save` (Preferences > Build) watches the project's `src` folder,
  only while the project's own build is used.
- Coverage: `BuildWorkflowTests` (operators, queueing, cancel, build on save),
  `OperatorTests` (the binding), `EditorSettingsTests`.

### C2. Code generators

- [x] Add operators `module.new_behaviour`, `module.new_component` and
      `module.new_system`. Each opens a dialog for the name, category and
      fields.
- [x] Snippets live in `templates/cpp_game/snippets/`. The generator inserts at
      the `# zenith:sources` marker in `src/CMakeLists.txt` and the
      `// zenith:register` marker in the registration file.
- [x] After generating, open the new file in the IDE through `ExternalEditor`
      and start a build.
- [x] Add the markers to the template. If a project lacks them, report a clear
      error.

Result:
- `src/editor/project/CodeGenerator.*` (`generateModuleCode`) and
  `TemplateText.*` (the `{{TOKEN}}` substitution, shared with the Project
  Hub). A third marker, `// zenith:includes`, places the new header's
  include.
- `operators/CodeOperators.*` is the `code` family: the three dialog
  operators and the headless `module.generate_code`. The dialog is
  `application/NewCodeDialog`.
- Coverage: `CodeGeneratorTests` (`zenith_code_generator`), and
  `zenith_project_template_module_build` builds one generated type of each
  kind.

Follow-ups:
- The registration file is always `src/GameModule.cpp`, and new files always
  go in `src/`; there is no setting for a project laid out differently.

### C3. Inspector integration for Behaviours

- [x] Show behaviours in Add Component under a "Behaviours" category.
- [x] Add an **Edit Script** button to each behaviour's section header. It
      opens the registered `source_location`.
- [x] Show a badge on the header while a build is running or after a failed
      reload.
- [x] Check that live Play inspection and Apply to Edit Scene work for
      behaviour fields.

Result:
- The category already came from `BehaviourOptions`; a test now pins it.
- `modules/EditorScriptInfo.hpp` is what the inspector asks: the session
  answers from `GameModuleHost::behaviourSource` and `ProjectBuild`, through
  `EditorContext::scripts`. `module.edit_script` is in the `code` family.
- Badges: Building, Build failed, and Reload failed. A failed reload
  unregisters the module's types, so that badge sits on the preserved
  "(unknown type)" sections instead.
- Coverage: `PropertiesPanelTests` (button, badges, Add Component),
  `LiveEditingTests` (`testBehaviourFieldsInPlay`), `CodeGeneratorTests`
  (`module.edit_script`).

### C4. Debugging tools

- [x] Add a `debug.attach` operator that runs the configurable
      `ide.debugCommand` with the editor's process id. Module PDBs are already
      shadow-copied.
- [x] Add gameplay log categories that can be filtered in the Info panel.
- [x] Show gameplay debug draw (from A2) in the game view.
- [x] Add a list of pinned fields in the Properties panel to watch during
      Play.

Result:
- `operators/DebugOperators.*` is the `debug` family: `debug.attach` (the
  setting is `ide.debug_command`), `debug.watch_field`,
  `debug.unwatch_field` and `debug.clear_watches`. `currentProcessId` is in
  `engine/platform/ChildProcess.hpp`, and `ExternalEditor` gained
  `expandToolCommand` for any placeholder set.
- `GameServices::log(name)` logs under `gameplay.<name>`; the Info panel's
  Categories dropdown hides categories (`InfoFilter::hiddenCategories`).
- Gameplay debug lines already drew in game views; their labels now do too.
- Watched fields are `interaction/WatchList.*`, shown by the Properties
  panel. The field context menu is now one `##FieldMenu` with Revert to
  Prefab and Watch Field.
- Coverage: `DebugToolsTests` (`zenith_debug_tools`), `InfoPanelTests`,
  `GameServicesTests`, `PrefabUiTests`, `EditorSettingsTests`.

Follow-ups:
- No test draws a game view's labels; that needs a rendered frame.
- The watch list is not saved with the session's other settings.

### C5. Project settings editors

- [x] Physics Layers tab: layer names and the collision matrix (A3).
- [x] Input Actions tab: actions and bindings, saved as the module's default
      action JSON (A4).
- [x] Tags.

Result:
- `panels/ProjectSettingsWindow.*` (`project.settings`, Edit menu) and the
  session's `EditorProjectSettings.cpp`. Docs: `docs/editor/project-settings.md`.
- Manifest version 6: `inputActions` (a file, `InputActionsJson.*`, whose
  actions override the module's) and `tags`. `ProjectApplication` and the
  editor's `GameModuleHost` apply the file.
- `TagsComponent` (`scene/Tags.*`) with `hasTag` and `findEntitiesWithTag`.
- `Fields::nameList` lets the inspector choose physics layers by name, which
  closes A3's follow-up.
- `EditorRenderSettings::save` re-reads the project file, so it no longer
  writes back stale layers.
- Coverage: `InputActionsJsonTests`, `TagsTests`, `ProjectConfigTests`,
  `ProjectSettingsTests`.

Follow-ups:
- A player's rebinding (`InputProfile`) is still not saved per game (A4).
- Removing a layer from the middle is not offered; it would need remapping
  every body's index.

### C6. FPS template and tutorial

- [x] Add `templates/fps_game/` with:
  - a player prefab (B1)
  - a hitscan weapon Behaviour (filtered ray, B4 effects)
  - a target Behaviour (triggers and events)
  - a HUD Ui system
  - a pause and settings screen (B6)
  - a test scene
- [x] Rewrite `docs/tutorials/fps-game/` around the module and editor
      workflow.

Exit criteria:
1. Create a project from the FPS template in the Project Hub.
2. Add a Behaviour with `module.new_behaviour` and build it with Ctrl+B; it
   appears in Add Component.
3. Play with F5: walk, shoot a target, and see the HUD update.
4. Run the same project with `zenith --project <file>`; it draws the HUD.

Result:
- Templates describe themselves in `template.json` (`ProjectTemplateInfo`,
  `listProjectTemplates`), and the Project Hub picks one. A template may ship
  its startup scene, input action file, and tags.
- `templates/fps_game/`: the player (character, camera with the
  `HitscanWeapon` behaviour), five tagged `Target`s, a `GoalZone` sensor, the
  `score` and `hud` systems with pause and settings menus, `input_actions.json`
  and `prefabs/impact.prefab`. `FpsTemplateTests` writes and checks the scene
  and prefab.
- `BehaviourContext::physics` lets behaviours raycast;
  `ZENITH_MODULE_API_VERSION` is 7.
- `docs/tutorials/fps-game/` is rewritten in six chapters around the hub,
  generators, Ctrl B, the inspector, Project Settings, and debugging.
- Exit criteria: `zenith_fps_template_module_build` generates a project from
  the template, generates a behaviour, component, and system into it, builds
  it, and plays the scene headless through `GameplayRunner`: a shot destroys a
  target and the HUD reads "Targets 1 / 5", the path `zenith --project`
  runs. Criteria 1 to 4 were not walked through by hand in the editor.

Follow-ups:
- A module cannot pause the simulation: the template's pause clears the
  player's action names, and physics keeps stepping. A pause request in
  `GameServices` would let a game stop its world.
- The pause menu has no Quit, because the same module runs in the editor,
  where `requestQuit` would close the editor.
- The template's settings are not saved between runs.

## Phase D: refactors and optimizations

These items are independent and can go into any batch.

- [x] **D1.** Split `src/engine/scene/PrefabSystem.cpp` (about 1775 lines) into
      id remapping, overrides, nested instances and instance queries.

  Result: `PrefabIdRemap.cpp`, `PrefabOverrides.cpp` (staging comparison,
  applying, and recording structural edits), `PrefabNesting.cpp` and
  `PrefabQueries.cpp`, with the shared helpers in the internal
  `PrefabSystemInternal.hpp` (namespace `prefab_detail`). `PrefabSystem.cpp`
  keeps instantiate, refresh, build and capture (about 550 lines).

  Follow-ups:
  - `sortOverrides` is duplicated in `editor/document/PrefabAuthoring.cpp`.
  - `matrixJson` is duplicated as `matrix` in `Prefab.cpp`.
- [x] **D2.** Split `src/editor/application/EditorSession.cpp` (about 1100
      lines): move file dialogs, autosave and recovery, and the main menu and
      status bar into their own units.

  Result: `EditorFileDialogs.cpp` (with the unsaved-changes workflow),
  `EditorRecovery.cpp`, `EditorMainMenu.cpp` and `EditorStatusBar.cpp`;
  entering and leaving Play mode and the game view's Ui moved to
  `EditorPlayback.cpp`. `EditorSession.cpp` is about 600 lines. The unused
  `drawPanels` declaration is gone.
- [x] **D3.** Split `src/engine/platform/FileWatcher.cpp` (about 1200 lines)
      into one file per backend: polling, Win32, inotify.

  Result: `FileWatchPolling.cpp`, `FileWatchWin32.cpp` and
  `FileWatchInotify.cpp`, with the shared sink, tree walk and path helpers in
  the internal `FileWatchBackends.*`. `FileWatcher.cpp` (about 350 lines)
  keeps the coalescer and the public class. The polling test's hang is under
  [Open questions](#open-questions-and-follow-ups).
- [x] **D4.** `SceneEditing::perform` serializes the whole scene twice per
      edit. Diff only the touched entities. Measure on a large scene first.

  Result: undo history already stored per-entity diffs. The cost was in
  recording each edit: two whole-scene serializations (three through the
  operator runner), a string compare, and a diff that parses both again.
  `tests/editor/SceneEditingBenchmark.cpp`
  (`zenith_scene_editing_benchmark_run`), 5,000 entities, Release means in
  milliseconds:

  | Measure | Before | After |
  | --- | --- | --- |
  | Inspector field edit | 120.28 | 1.67 |
  | Gizmo drag frame | 116.06 | 1.51 |
  | Undoable operator (runner pattern) | 138.96 | 108.17 |
  | Undo / redo | 0.59 / 0.61 | 0.63 / 0.65 |

  - `SceneEditing::captureEntities` and `commit(EntitySnapshot)` serialize
    only the listed entities, plus any the edit created, through
    `SceneDocument::serializeEntities` (D6). They are used by the
    inspector's name, transform and component edits, and by the viewport's
    transform widget and gizmo.
  - Structural edits and operators keep the whole-scene capture. The runner
    passes its own after-capture (`commit(name, key, before, after)`).
  - `testRandomizedEquivalence` runs its field edits both ways against full
    snapshots; `testEntityCaptureGuards` covers created entities, an
    unlisted destroy, and the runtime scene.

  Follow-ups:
  - About 1 ms of a recorded edit, and of a prefab spawn (D6), still grows
    with the scene. The likely cause, not yet profiled, is that
    `SceneDocument::applyChanges` validates every collection reference of the
    resulting scene.
  - Modal G/R/S and the code paths that create objects (duplicate, place
    model) take their `before` as a whole-scene string;
    `beginModalTransform`'s string parameter would have to change for them
    to capture entities.
- [x] **D5.** Add a shared versioned-settings JSON helper. Adopt it in
      GameSettings, EditorPreferences, InputProfiles, ViewportChrome,
      HierarchyPanel, EditorAreas and PlatformServices.

  Result: `engine/platform/VersionedJson.*` (`zenith_versioned_json`), and an
  explicit `UserDataBackup` choice for `atomicWriteUserData`, which now also
  takes a bare file name. Adopted in:
  - GameSettings and InputProfiles;
  - PlatformServices (diagnostic bundle, release provenance);
  - the editor's `WorkspaceFile` (EditorAreas), `AssetBrowserSettings`,
    `ComponentInspectorLayout`, `SimulateSystemSettings`, `RecentProjects`
    and `SceneEditorData`.

  The three editor settings files that overwrote themselves in place are now
  written atomically. Only the platform files keep a `.bak`, as before.

  Not adopted:
  - ViewportChrome, HierarchyPanel and EditorPreferences (see Decisions).
  - `Keymap`: its public `serializeOverrides`/`loadOverrides` work on text,
    and it deletes its file when nothing is overridden. Adopting it would mean
    changing that API for little gain.
- [x] **D6.** Measure prefab compare and refresh through the staging scene.
      Optimize only if it shows up in the measurements.

  Result: `tests/engine/PrefabBenchmark.cpp`
  (`zenith_prefab_benchmark_run`). The scene has 5,000 background entities
  and ten instances of a 200-entity prefab with a nested prefab, 7,050
  entities in all. Release means, in milliseconds:

  | Measure | Before | After |
  | --- | --- | --- |
  | Spawn a 3-entity prefab into that scene | 75.99 | 1.68 |
  | Spawn it into an empty scene | 1.03 | 0.82 |
  | `refresh` (keep added objects) | 97.05 | 25.59 |
  | Apply pattern (compare 9 instances, capture, refresh 9) | 991.77 | 315.39 |
  | `overrides` against a prepared source | 6.50 | 6.93 |
  | `entityOverrides` (each drag frame) | 0.09 | 0.14 |
  | `prepareComparison` | 9.60 | 9.49 |

  - Spawning and refreshing serialized the whole scene before every change,
    just to be able to roll a failure back (`PrefabSystem::transact`). Now
    only the instance's subtree is saved, and the entities the change
    created are found by id (`Scene::nextEntityId`). The new public
    `SceneDocument::serializeEntities` writes the chosen entities in diff
    form.
  - `PrefabAuthoring::apply` prepares the previous prefab once for all the
    other instances, instead of normalizing it again for each.
  - Comparison is left as it is: the per-frame `entityOverrides` costs a
    tenth of a millisecond.
  - `testFailedRefreshRestoresScene` (`zenith_prefabs`) covers the rollback.

  Follow-ups:
  - A spawn into a large scene still costs about twice one into an empty
    scene; what remains grows with the scene, not with the prefab.
  - `instantiate` and `refresh` still run the full build twice, once in the
    staging scene and once for real.
- [x] **D7.** Docs hygiene:
  - `docs/PERFORMANCE_ROADMAP.md` is linked from `ARCHITECTURE.md`,
    `NEXT_STEPS.md` and `REPOSITORY_GUIDE.md` but does not exist. Restore it
    or remove the links.
  - Fix the stale controller line in `docs/engine/physics.md`.

  Result: the file is restored unchanged from before 92c6498, which deleted
  it by mistake; `README.md` links it too. The physics line was already
  fixed in A3. `REPOSITORY_GUIDE.md` no longer lists the removed
  `src/game/player/`.

## Phase E: roguelike foundations

Target: a project created from the FPS template can do all of the following
without engine changes:
- generate a floor of room prefabs from a seed;
- fill it with enemies that path to the player without piling up;
- play hit and pickup sounds;
- pause for an inventory screen;
- save the run and resume it on the same floor;
- replace the floor with the next one.

Read first: Phase A and B docs plus [Navigation](engine/navigation.md),
[Audio](engine/audio.md) and [Gameplay](engine/gameplay.md).

### E1. Seeded gameplay random streams

- [x] Add a small, fast, portable generator (for example PCG32 or
      xoshiro128**) with explicit state, and helpers for ranges, floats,
      weighted picks and shuffles. Its results must not depend on the
      standard library's distributions, which differ between implementations.
- [x] `GameServices::random(name)` returns a named stream (`layout`, `loot`,
      `combat`, ...) derived from the session seed and the name. Drawing from
      one stream never changes another.
- [x] The seed and every stream's state can be read and restored, for E2.

Today the only randomness is `std::random_device` (editor play targets, asset
GUIDs); there is nothing seeded for gameplay.

Exit: the same seed gives the same sequences on every run and build type,
and stream independence is tested.

Result:
- `engine/gameplay/Random.*`:
  - `RandomStream` is PCG32 with its own `below`, `range`, `unit`, `chance`,
    `weightedIndex` and `shuffle`;
  - `RandomStreams` holds named streams (SplitMix64 over the seed and the
    name's FNV-1a hash), with `states()` and `restore(seed, states)`;
  - `entropySeed()` gives a seed for a new run.
- `GameServices::random(name)` and `randomStreams()`. The session seed
  starts at 0, and a game reseeds it when a run begins.
- Coverage: `tests/engine/RandomTests.cpp` (`zenith_random`).
  - PCG32's published reference sequence, cross-checked with an independent
    Python port.
  - Golden values for the named-stream derivation.
  - Helper ranges and distributions, stream independence, and restore.
- Every build type: integer results are exact by construction. Float helpers
  also need IEEE single precision without FMA contraction, which is MSVC's
  default; a GCC or Clang build would need `-ffp-contract=off`.

Follow-ups:
- The CMake toolchains do not set `-ffp-contract=off` yet. Add it when a
  GCC or Clang build ships.

### E2. Run and profile save data

- [x] `SaveGameStore` can save and load a typed payload that is not a scene:
      a reflected struct serialized through `Fields<T>`, with a version and
      migration, written through `atomicWriteUserData` like the scene slots.
- [x] A run slot holds the seed, the E1 stream states and the game's own run
      struct (floor index, the player's stats and inventory). A game
      regenerates the floor from them.
- [x] A separate profile file for meta-progression that a game never
      deletes on death, using the D5 `VersionedJson` helper.

Scene-diff saves (B5) stay as they are for hand-built levels. Deleting the run
slot on death is game policy.

Result:
- `SaveGameStore::saveData`/`loadData` take a reflected struct, a
  `SaveDataFormat` (`kind`, `version`, `migrate`) and a title.
  - Slots record their kind (`SaveGameInfo::kind`; scene saves write
    `scene`, and older saves read as `scene`). Neither kind loads as the
    other.
  - Loading reads into a default-constructed struct, upgrades older versions
    through `migrate`, refuses newer ones, and leaves the target unchanged on
    failure.
- `saveProfile`/`loadProfile` keep `<name>.zprofile` through
  `VersionedJson`, with a backup. `list` and `remove` ignore it, and a
  missing profile keeps the caller's defaults.
- Reflection gains `FieldKind::UInt64` (JSON keeps every bit; the inspector
  edits it as an unsigned 64-bit number) for seeds and stream states.
- E1's `RandomStreams::states()`/`restore(seed, states)` became
  `snapshot()`/`restore(snapshot)`, returning a reflected
  `RandomStreamsSnapshot` (`randomStreamsSnapshotFields()`). A run struct
  embeds it as an ordinary field.
- Coverage:
  - `testRunData`, `testDataVersions` and `testProfiles` in
    `SaveGameTests`;
  - the snapshot round trip in `RandomTests`.

Follow-ups:
- `GameServices` has no run-save shorthand. A game calls
  `saveGames()->saveData` with `randomStreams().snapshot()` itself; add a
  helper if every game repeats it.

### E3. Definition tables

- [x] Load a JSON array of a reflected struct into a table keyed by a
      string id: `DefinitionTable<T>`, with lookups by id and iteration in
      file order.
- [x] Report a duplicate id, an unknown field, a wrong type and a missing
      required field with the file, the entry's index and the field name.
- [x] Reload a changed table in the editor through the file watcher, when it
      validates.

The game defines the schemas (items, enemies, loot tables, floor themes).
Today content can come only from prefabs and components.

Result:
- Reflection gains `FieldKind::List`, through `Fields::list`. A loot table's
  drops could not be described without it.
  - A list is a `std::vector` of nested structs, written as an array of
    objects. Reading one replaces the list, and each element starts from its
    defaults.
  - Covered by validation and entity-reference remapping.
  - The inspector edits a list on one selected object: add, remove, and each
    element's fields. Several selected objects show only the counts.
- `engine/gameplay/DefinitionTable.*`:
  - `DefinitionTable<Entry>` has `load`, `parse`, `find(id)`, `entries()`
    and `reloadIfChanged()`.
  - The JSON work is in a non-template core (`definition_detail`), so the
    header stays free of nlohmann.
  - Loading is strict: unknown keys at any depth are errors, unlike scene
    documents. Errors read like
    `loot.json[0] 'crypt': unknown field 'drops[1].wieght'`. A failed load
    keeps the previous entries.
- Reloading compares the file's write time and size (`reloadIfChanged`)
  instead of using the editor's file watcher. It works the same in the
  editor and in a shipped game, needs no editor wiring, and a few stat calls
  a second cost nothing. See Decisions.
- Coverage:
  - `tests/engine/DefinitionTableTests.cpp` (`zenith_definition_tables`):
    lookups, lists, every error, keep-on-failure and reload;
  - `testLists` in `ReflectionTests`.

Follow-ups:
- No lists of plain values (strings, numbers) yet; a list of one-field
  structs works meanwhile.
- The eyedropper cannot pick an entity reference inside a list.
- Reflection still ignores unknown keys in scene documents. That is
  deliberate, so newer documents stay readable.

### E4. Building and replacing a floor at runtime

- [ ] Measure first. Spawn about 40 room prefabs with static triangle-mesh
      colliders through `EntityCommandBuffer`, then time the spawn, the
      physics bodies' creation and a navmesh bake. Record the numbers here
      and optimize only what shows up.
- [ ] Let a game rebake navigation on demand after it builds a floor.
      `GameServices::navigation` bakes once, lazily, today.
- [ ] Replace a floor cleanly: destroying a floor's entities releases their
      bodies, navigation and voices, and leaves the player and the game's
      persistent entities alone. Test that repeated build and replace cycles
      leave no bodies or voices behind.

The layout generator, room connectors and spawn tables are the game's.

### E5. Partial paths and crowd avoidance

- [ ] `findPath` returns the path to the closest reachable point when the
      goal is unreachable, and says so. It fails on `DT_PARTIAL_RESULT`
      today.
- [ ] Build DetourCrowd into `zenith_recast` and let `NavAgentComponent`
      agents avoid each other: radius, separation and a maximum agent count
      in the navigation settings.
- [ ] Rebuild the crowd when E4 rebakes the mesh.

Several agent sizes and area costs wait for the first enemy that needs them.

### E6. Audio for gameplay

- [x] `GameServices` (and so `BehaviourContext`) gives gameplay the session's
      audio: play, play at a position or following an entity, and stop, by
      handle. Null when the host has no audio, as in tests.
- [x] Reflected component types for `AudioEmitterComponent` and
      `AudioListenerComponent`, so scenes and prefabs can carry sounds. This
      closes B8's follow-up.

Result:
- `GameServices::setAudio`, `audio()` and `loadClip(assetPath)`. A clip path
  resolves against the session's asset root, and paths outside it are
  rejected. `GameplayRunner::start` and the editor's Play entry give the
  services the application's audio.
- `audio_listener` and `audio_emitter` component types
  (`scene/AudioComponentTypes.cpp`).
  - The emitter's sound is `clip_asset`, saved as an asset reference with
    its GUID; the `clip` handle stays runtime-only.
  - The new built-in system `zenith.audio_emitters` (LateUpdate, Play) loads
    an emitter's clip whenever the path changes. A clip that cannot load is
    logged once.
  - Emitters do not sound in Edit mode.
- E7's follow-up is closed: the playback rate now applies only to
  `AudioScope::Scene` voices, so menu sounds and music (Persistent) play on
  while the game is paused.
- Coverage:
  - `testGameplayAudio` in `AudioSystemTests`: services clips, path safety,
    emitter loading and reporting, and the components' document round trip;
  - the playback-rate test, which now checks that a persistent voice plays
    on.
  - The schedule tests list the new system.

Follow-ups:
- The inspector edits `clip_asset` as text. Dropping a clip from the Asset
  Browser onto it would be friendlier.
- Emitters could preview in Edit mode. They load only while gameplay runs,
  because the clip cache lives in the application's audio, and loading
  belongs to the session.

### E7. Pause, teardown and one game clock

- [x] A pause request in `GameServices` stops the simulated clock, physics,
      timers and FixedUpdate. Ui systems keep running so menus work. The FPS
      template uses it instead of clearing the player's actions (C6
      follow-up).
- [x] Stopping a session or unloading a scene runs `onDisable` and
      `onDestroy` for every live behaviour (B2 follow-up).
- [x] Update systems, behaviours, timers, animation and the animator all
      advance on simulated time, with the unscaled frame time available
      separately (B2 and B3 follow-ups).
- [x] A hook that throws is caught. The entity and behaviour are logged, and
      the behaviour is disabled (B2 follow-up).

Result:
- `GameServices::setPaused`, `setTimeScale` and `simulationClock(host)`. The
  game's clock combines with the host's: the scales multiply, and the game's
  pause holds even through the editor's frame step.
  - `GameplayRunner::simulationClock` is what the game's scenes
    (`ProjectApplication`, `GameplayScene`) report to the engine.
  - `EditorSession::simulationClock` applies it on top of `EditorPlayMode`.
- While paused, Update and LateUpdate still run with a zero `deltaSeconds`,
  so a game can read its pause key and resume. Without that, a pause could
  never end.
  - `SystemContext` and `BehaviourContext` gain `unscaledDeltaSeconds`.
  - Ui systems get the wall-clock frame time in both hosts (the editor
    passed zero before).
  - The first-person camera ignores look input while the game is paused.
- The game host now runs Update, LateUpdate and the timers on the game's
  clock. The editor already did. The animator and `updateAnimations` share
  one clock, which closes the B2 and B3 time-base follow-ups.
- `destroyAllBehaviours` ends every created behaviour in run order and marks
  it uncreated. It runs from:
  - `GameplayRunner::stop(context)` (from each scene's `unload`);
  - `GameModuleHost::stopGameplay(scene)` (Stop, and module release, which
    also ends behaviours that ran in Edit mode).
- Every hook call goes through one guard. It logs
  `Behaviour '<key>' on entity <id> (<name>) threw in <hook>: <what>` and
  disables the behaviour, so the lifecycle calls `onDisable` on the next
  pass. Failures in `onDisable` and `onDestroy` are logged only.
- The FPS template pauses through `setPaused`. Its saved action names are
  gone.
- `ZENITH_MODULE_API_VERSION` is 8.
- Coverage:
  - `testSessionEnd` and `testThrowingHooks` in `BehaviourTests`;
  - `testGameClock` in `GameServicesTests`;
  - `zenith_fps_template_module_build`, which now checks that a paused
    player neither walks nor looks, and that the game resumes.

Follow-ups:
- ~~A paused clock sets the audio playback rate to zero for every voice, so
  menu sounds and music freeze with the world.~~ Closed in E6: the rate
  applies to Scene-scope voices only.
- A system (not a behaviour) that throws still propagates. Only behaviour
  hooks are guarded.

### Not in Phase E

Each of these waits until the roguelike's first playable slice shows it is
needed:
- inventory slot grids, drag and drop, tooltips;
- world-space UI (damage numbers, health bars, prompts);
- the game font in the editor's game view;
- `ButtonBehavior::Toggle`, an any-key query and full rebinding;
- animation blend trees and layers;
- particle flipbooks and trails;
- hinge constraints and an angular velocity API;
- a deterministic replay harness.

## Conventions for every item

- Configure worktrees with `ZENITH_SHARED_DEPS_DIR` and `cmake --preset
  worktree` (see `AGENTS.md`).
- Run `tools/check.ps1` before handing off, and `tools/check.ps1 -Full` at the
  end of a batch.
- New editor features are operators and registry-driven (see
  [Operators and keymaps](editor/operators.md)).
- Register new tests above the per-build `test-temp` fixture block in
  `tests/*/CMakeLists.txt`.
- New editor smoke steps must use a free frame window (see
  [Editor smoke](editor/smoke.md)).
- Graphics or UI changes also need a bounded Vulkan smoke run
  (`ZENITH_SMOKE_FRAMES`).
- Changing a serialized field's key or meaning needs a schema version and a
  migration.

## Open questions and follow-ups

- `zenith_file_watcher_polling` hung twice under the parallel CPU suite while
  Phase B landed, though it passes alone in about 3 s. D3 found no deadlock in
  the polling backend: it has no lock cycle and every wait is bounded. The
  likelier cause was the test, which renamed and deleted with the throwing
  `std::filesystem` calls while the polling walk held search handles in the
  same folders. On Windows that can fail; the uncaught exception aborts, and a
  Debug build's abort dialog waits forever. The test now retries those calls
  and fails instead of aborting, and both watcher tests have a 60 s timeout.
  Eight repeated parallel suite runs passed. Since the hang was never
  reproduced on demand, reopen this if it comes back.

- `zenith_showcase_capture_fsr1_upscale` failed once in the Phase D
  `check.ps1 -Full` run and passed alone four times. It is a GPU reference
  capture that Phase D's CPU-side changes do not reach, so it is likely a
  flake under parallel Vulkan load.

- Phase E batch 1's `check.ps1 -Full`, run after the "8-bit Blender"
  retheme merged (a430726), had two failures in 206 tests, neither from
  Phase E:
  - `zenith_scene_gizmos_capture_overlays` mismatches its reference
    (1.3% of pixels): gizmo colours come from `EditorPalette`, which the
    retheme changed, so the reference needs regenerating.
  - `zenith_production_milestones` failed under the parallel suite and
    passed alone.

- Hot reload during Play is out of scope for now. Reloads stay queued until
  Stop.
