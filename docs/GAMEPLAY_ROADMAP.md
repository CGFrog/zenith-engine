# Gameplay roadmap

This roadmap makes a typical first-person game programmable end to end from
the editor, as a hot-reloaded C++ game module. It follows the completed editor
roadmap (reflection, module hot reload, operators, workspaces, prefabs, play
targets). Each item is sized so that one session, or one `/batch` unit, can
finish it.

Approved 2026-10-04. Nothing is implemented yet.

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
| B1 | Engine character controller | A3, A2 | todo |
| B2 | Behaviours | A2, A3 | todo |
| B3 | Animation state machine | A2 | todo |
| B4 | Effects and pooling | A2 | todo |
| B5 | Save game | A2 | todo |
| B6 | Game UI widgets and settings screen | A2 | todo |
| B7 | Navigation | A3 | todo |
| B8 | Audio polish | A3 | todo |
| C1 | Build as an operator | — | todo |
| C2 | Code generators | B2, C1 | todo |
| C3 | Inspector integration for Behaviours | B2 | todo |
| C4 | Debugging tools | A2 | todo |
| C5 | Project settings editors | A3, A4 | todo |
| C6 | FPS template and tutorial | B1–B6, C2, C3 | todo |
| D1 | Split `PrefabSystem.cpp` | — | todo |
| D2 | Split `EditorSession.cpp` | — | todo |
| D3 | Split `FileWatcher.cpp` per backend | — | todo |
| D4 | Targeted diffs in `SceneEditing` | — | todo |
| D5 | Shared versioned-settings helper | — | todo |
| D6 | Measure prefab compare/refresh cost | — | todo |
| D7 | Docs hygiene | — | todo |

Suggested batches:

1. **A1–A4, C1, D7.** Small and independent; they remove every hard blocker.
2. **B1, B2, C2, C3.** They deliver the scripting model.
3. **B3–B6, C4–C6.** They deliver the FPS template.

B7, B8 and D1–D6 fit in anywhere. Measure D4 and D6 before changing code.

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

- [ ] Add Recast/Detour behind a `NavigationWorld`, with navmesh baking from
      static colliders and a `NavAgentComponent` that follows paths.
- [ ] Add an editor bake operator, with the navmesh shown through `DebugDraw`.

### B8. Audio polish

- [ ] One-shot sounds that follow an entity.
- [ ] Optional occlusion using an A3 filtered raycast.

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

- [ ] Add `build.run` (Ctrl+B) and `build.cancel` operators. The Build panel
      button (`src/editor/panels/BuildPanel.cpp`) calls them.
- [ ] Add an optional build-on-save preference. It uses `FileWatcher` on the
      module's source directory, debounced like `GameModuleHost`.
- [ ] When a build finishes during Play, show "Reload pending" in the status
      bar. The reload already waits for Stop.

### C2. Code generators

- [ ] Add operators `module.new_behaviour`, `module.new_component` and
      `module.new_system`. Each opens a dialog for the name, category and
      fields.
- [ ] Snippets live in `templates/cpp_game/snippets/`. The generator inserts at
      the `# zenith:sources` marker in `src/CMakeLists.txt` and the
      `// zenith:register` marker in the registration file.
- [ ] After generating, open the new file in the IDE through `ExternalEditor`
      and start a build.
- [ ] Add the markers to the template. If a project lacks them, report a clear
      error.

### C3. Inspector integration for Behaviours

- [ ] Show behaviours in Add Component under a "Behaviours" category.
- [ ] Add an **Edit Script** button to each behaviour's section header. It
      opens the registered `source_location`.
- [ ] Show a badge on the header while a build is running or after a failed
      reload.
- [ ] Check that live Play inspection and Apply to Edit Scene work for
      behaviour fields.

### C4. Debugging tools

- [ ] Add a `debug.attach` operator that runs the configurable
      `ide.debugCommand` with the editor's process id. Module PDBs are already
      shadow-copied.
- [ ] Add gameplay log categories that can be filtered in the Info panel.
- [ ] Show gameplay debug draw (from A2) in the game view.
- [ ] Add a list of pinned fields in the Properties panel to watch during
      Play.

### C5. Project settings editors

- [ ] Physics Layers tab: layer names and the collision matrix (A3).
- [ ] Input Actions tab: actions and bindings, saved as the module's default
      action JSON (A4).
- [ ] Tags.

### C6. FPS template and tutorial

- [ ] Add `templates/fps_game/` with:
  - a player prefab (B1)
  - a hitscan weapon Behaviour (filtered ray, B4 effects)
  - a target Behaviour (triggers and events)
  - a HUD Ui system
  - a pause and settings screen (B6)
  - a test scene
- [ ] Rewrite `docs/tutorials/fps-game/` around the module and editor
      workflow.

Exit criteria:
1. Create a project from the FPS template in the Project Hub.
2. Add a Behaviour with `module.new_behaviour` and build it with Ctrl+B; it
   appears in Add Component.
3. Play with F5: walk, shoot a target, and see the HUD update.
4. Run the same project with `zenith --project <file>`; it draws the HUD.

## Phase D: refactors and optimizations

These items are independent and can go into any batch.

- [ ] **D1.** Split `src/engine/scene/PrefabSystem.cpp` (about 1775 lines) into
      id remapping, overrides, nested instances and instance queries.
- [ ] **D2.** Split `src/editor/application/EditorSession.cpp` (about 1100
      lines): move file dialogs, autosave and recovery, and the main menu and
      status bar into their own units.
- [ ] **D3.** Split `src/engine/platform/FileWatcher.cpp` (about 1200 lines)
      into one file per backend: polling, Win32, inotify.
- [ ] **D4.** `SceneEditing::perform` serializes the whole scene twice per
      edit. Diff only the touched entities. Measure on a large scene first.
- [ ] **D5.** Add a shared versioned-settings JSON helper. Adopt it in
      GameSettings, EditorPreferences, InputProfiles, ViewportChrome,
      HierarchyPanel, EditorAreas and PlatformServices.
- [ ] **D6.** Measure prefab compare and refresh through the staging scene.
      Optimize only if it shows up in the measurements.
- [ ] **D7.** Docs hygiene:
  - `docs/PERFORMANCE_ROADMAP.md` is linked from `ARCHITECTURE.md`,
    `NEXT_STEPS.md` and `REPOSITORY_GUIDE.md` but does not exist. Restore it
    or remove the links.
  - Fix the stale controller line in `docs/engine/physics.md`.

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

- Hot reload during Play is out of scope for now. Reloads stay queued until
  Stop.
