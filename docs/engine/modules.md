# Game modules (`src/engine/module/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Game modules: shared libraries that add component types and systems to a host.
`ModuleApi.hpp` is the C-linkage contract (`ZENITH_GAME_MODULE` defines
`zenith_module_api_version`, `zenith_module_build_hash`, name, and version; the
module defines `zenith_module_register(ModuleRegistrar&)`), and
`checkModuleCompatibility` refuses another API version or toolchain/runtime
build hash. `GameModule::load` copies the library (and its PDB, patching the
copy to name it) to a unique shadow file, or with
`GameModuleLoading::InPlace` (a packaged game) uses the library itself, loads that with
`SharedLibrary` (`LoadLibrary`/`dlopen`), and registers into a copy of the
built-in (or host-supplied) `ComponentTypeRegistry` plus its own
`SystemRegistry`, recording which component keys the module added. Every
registry starts with the engine's own systems (`BuiltInSystems.*`:
`zenith.character_input` and `zenith.first_person_camera`, see
[Gameplay](gameplay.md), the three behaviour systems, `zenith.nav_agents`
(see [Navigation](navigation.md)), and `zenith.animator`,
see [Animation](animation.md)), so they run before a game's systems of the
same phase and order (the animator is ordered after them).
`SystemScheduler` runs a registry's systems per `SystemPhase` (`Update`,
`FixedUpdate`, `LateUpdate`, `Ui`) by ascending order, filtered by `RunIn`
edit/play flags; in `SimulationMode::Simulate` it runs the Play systems its
`SimulateSystemMask` enables (default: FixedUpdate on, every other phase off). An optional `SystemProfile`
passed to `run` records per-system CPU time (frame, rolling average, maximum). Everything a
module registered must be released, and every scene its components touched
destroyed, before the `GameModule` is destroyed; otherwise the library is
deliberately left loaded. `Gameplay.hpp` is what a game plays: a component
type registry and a `SystemScheduler`, from a module (`gameplayFromModule`),
from registration linked into the executable (`linkGameplay`), or the built-in
types and systems alone. `GameplayRunner` plays a `Gameplay` in a game's
`ApplicationScene` (the project application and the demo use it).

## Fault isolation

A gameplay bug must not end the game or the editor, so every call into game
code is guarded (`GuardedCall.hpp`, `callGuarded`):

- `SystemScheduler::run` catches whatever a system throws, logs it as an
  error in the `gameplay` category (system name, phase, and `what()`), and
  goes on with the next system. Given a `SystemFaults`, it also disables the
  system there and skips it from then on, reporting it only once, like a
  behaviour hook that throws (`BehaviourScheduler`). The scheduler is shared
  and immutable, so the disabled set is the host's per-session state beside
  its `SystemProfile`: `GameplayRunner` clears its own on `start` and `stop`
  (`systemFaults()`), and the editor's `GameModuleHost` clears its own when
  gameplay starts or stops and when the module loads or reloads.
- `Timers::advance` logs a throwing callback to its `LogChannel` and drops
  its timer, even a repeating one; the other timers run.
- `EntityCommandBuffer` logs a throwing `onSpawned` callback; the entity
  stays and the later commands apply. `Effects` runs no game code itself: its
  spawns and lifetimes go through these two.

Structured exceptions (access violations, stack overflows) are not C++
exceptions; the process's crash handler records them (see
[Platform](platform.md#srcengineplatformcrashhandler)).
`EngineExport.hpp` defines `ZENITH_ENGINE_API` for data that must cross the
engine library boundary.

## Frame order and phases

Every host runs a frame's systems in this order (`Engine::run` through the
`ApplicationScene` hooks; the editor's Play mode through `EditorSession`):

1. `GameServices::beginFrame`: last frame's events become readable.
2. `Update` with the frame's input, then the timers due on the simulation
   clock.
3. Per fixed step: `FixedUpdate` with the step's input, the physics step, then
   `ApplicationScene::afterPhysicsStep`.
4. Physics interpolation, then `LateUpdate` (cameras and anything that follows
   a body) with the frame's input.
5. World transforms are finalized and the scene renders.
6. `Ui` inside the UI frame, drawing through `SystemContext::ui` (the
   application's `GameUiContext`; in the editor, one sized to the game view
   and drawn over it).

Recorded entity commands flush after each phase run and after the timers
(`runGameplayPhase`), so systems never change the scene under a view another
system of the same phase is walking.

## `SystemContext`

Besides the scene, mode, and delta, a system gets:

- `logger`: always set when a host runs it, in Edit too.
- `input`: null when no player input reaches the scene (Edit, Simulate).
- `services`: the session's `GameServices`, null in Edit.
- `ui`: set only in the `Ui` phase.
- `actions`: the module's input actions (`ModuleRegistrar::addInputActions`)
  evaluated by the host from `input`, and null whenever `input` is. See
  [Window and input](window-input.md).

Changing `SystemContext`, `ModuleRegistrar`, or a registered descriptor bumps
`ZENITH_MODULE_API_VERSION` (now 8: the gameplay roadmap's A2 changed `SystemContext`, A3 `RigidBodyComponent::layer`, A4 `ModuleRegistrar`, B1 made `character_controller` an engine component type with engine systems, B2 added behaviours to `ModuleRegistrar` and `SystemContext`, C6 added `BehaviourContext::physics`, and E7 added `unscaledDeltaSeconds` to both contexts and the pause and time scale to `GameServices`).

## Behaviours (`src/engine/behaviour/`)

A `Behaviour` is per-entity logic: a class derived from `Behaviour` with
virtual hooks, registered with
`registrar.addBehaviour<T>(key, displayName, Fields<T>()..., BehaviourOptions)`.
The registrar builds T's component type (category "Behaviours" unless the
options name another, an optional `enabled` field before T's own), so the
inspector, serialization, multi-edit, undo, prefab overrides and preserved
JSON all work unchanged. It also records a `BehaviourType` in the gameplay's
`BehaviourRegistry`: type-erased functions to find T's entities and cast them
to `Behaviour`, the `RunIn` flags (Play unless the type opts into Edit), and
the `std::source_location` of the registration.

The engine's systems `zenith.behaviours` (Update), `zenith.fixed_behaviours`
(FixedUpdate) and `zenith.late_behaviours` (LateUpdate) call `runBehaviours`
with `SystemContext::behaviours`. Each pass:

1. Runs the lifecycle hooks that are due: `onCreate` for new behaviours, then
   `onEnable`/`onDisable` when `enabled` changed, then `onStart` once for each
   enabled behaviour that has not started.
2. In FixedUpdate, runs `onTriggerEnter`/`onTriggerExit` (both sides of a
   sensor event) and `onCollision` (both sides of a new contact between solid
   bodies) for the physics step since the last pass. The host hands the step's
   events to `GameServices::publishPhysicsEvents`, which keeps them by scene id.
3. Runs the phase's hook (`onUpdate`, `onFixedUpdate`, `onLateUpdate`) on
   enabled behaviours.

Types run in registration order and each type's entities in creation order
(scene id), so the order is deterministic. Hooks receive a `BehaviourContext`:
the scene, entity, delta on the simulated clock and on the wall clock
(`unscaledDeltaSeconds`), `GameServices` (null in Edit), actions, logger, and
the physics world for queries (null in Edit).
Hooks change entities through `GameServices::commands()`; when a destroy
command applies, `GameServices` calls `onDisable` and `onDestroy` on the
subtree's behaviours first (`destroyBehaviours`). When a session stops or its
scene unloads, the host calls them on every created behaviour, in run order,
before it releases the services (`destroyAllBehaviours`, from
`GameplayRunner::stop` and `GameModuleHost::stopGameplay`, which a module
reload also runs for behaviours that ran in Edit mode); the behaviours count
as uncreated afterwards. A hook that throws is caught: the `gameplay` log gets
an error naming the behaviour type, the entity, the hook, and the exception,
and the behaviour is disabled, so it gets `onDisable` on the next pass and no
more updates. The other behaviours and the session carry on. A copied behaviour (a
duplicate, a prefab instance, the Play copy of the scene) starts uncreated;
assignment (undo, Apply to Edit Scene) keeps the target's lifecycle state.
Behaviours survive a module reload as any component does: by their serialized
fields. Their lifecycle state does not, so a reloaded behaviour is created
and started again.

### Behaviour or system?

- Use a **Behaviour** for logic that belongs to one entity and reacts to its
  own events: a door, a pickup, a weapon, an enemy's decisions. Designers add
  it in the inspector and tune its fields per entity.
- Use a **system** for logic over many entities at once, or between them:
  movement integration, AI scheduling across agents, scoring, a HUD. Systems
  iterate component views directly and are cheaper per entity.
- A behaviour may read anything a system can; when its per-entity work grows
  hot, move the work into a system over a plain component and keep the
  behaviour as configuration.

## `GameServices`

`log()` is the `gameplay` log channel; `log(name)` is a channel for one part
of the game, category `gameplay.<name>`, which the editor's Info panel can
show or hide on its own.

`GameServices` is owned by whatever runs gameplay:
- `GameplayRunner`, from a game scene's load to its unload;
- `GameModuleHost` in the editor, from entering Play or Simulate to stopping.
  The host also stops it before the module unloads or reloads.

It holds:

- `PrefabSystem` over the gameplay's component types and asset root.
- `EntityCommandBuffer`: `spawn` (a prefab), `spawnEmpty`, `destroy`, and
  `reparent` by scene id, applied in order at the flush points. A spawn's
  callback configures the new entity. Spawned bodies are marked for physics.
  Flushes publish `EntitySpawned` and `EntityDestroyed`. Named so that it does
  not collide with the editor's `EntityCommands` namespace.
- `EventQueue`: typed events, each with a unique
  `static constexpr std::string_view eventKey`. An event published in frame N
  is readable in every phase of frame N + 1, then dropped. Channels are keyed
  by that string, not a C++ type id, because the engine and each module have
  their own template statics. Engine events (`GameEvents.hpp`):
  - `SceneLoaded`, published when gameplay starts;
  - `EntitySpawned` and `EntityDestroyed`;
  - `TriggerEntered` and `TriggerExited`.
- The physics events of the last step for the behaviours' trigger and
  collision hooks (`publishPhysicsEvents`, `takePhysicsEvents`).
- `Effects`: `spawn(prefab, transform, EffectOptions)` places a prefab that
  removes itself when it ends: after an explicit lifetime, or the longest
  particle lifetime of its one-shot emitters (a continuous or repeating
  emitter needs an explicit lifetime). Lifetimes run on the session's timers.
  A pooled effect parks its instance (emitters stopped) instead of being
  destroyed, and the next pooled spawn of that prefab moves and restarts it,
  which avoids restarting every emitter in the scene as entity changes do.
  Spawns apply at the next flush, before the entity commands.
- Save games, when the host provides them (`setSaveGames`): `saveGames()`
  and `saveGame(scene, slot, title)` against the scene document the session
  plays (see [Scene](scene.md)).
- `navigation(scene)`: the session's navigation mesh, baked from the scene's
  static colliders on first use. `bakeNavigation(scene, settings)` bakes it
  again now, for a game that builds or replaces a level at runtime; call it
  after the spawn commands applied (the next phase, or a timer).
- `Timers`: one-shot or repeating callbacks on simulated time. They follow the
  clock's time scale and stop while paused. A timer may have an owner entity;
  the timer is dropped when its owner is gone, or when its callback throws.
- The game's own clock: `setPaused` and `setTimeScale`, for menus, inventory
  screens, and slow motion. `simulationClock(host)` combines them with the
  host's clock (the editor's pause, frame step, and time scale): the scales
  multiply and the game's pause holds even through a frame step. Hosts report
  the result as their `ApplicationScene::simulationClock`, so the engine
  stops physics and FixedUpdate, animation, and scene audio with it, and run
  Update and LateUpdate systems and the timers on its `simulatedSeconds`. A
  paused game's Update and LateUpdate systems still run, with a zero
  `deltaSeconds`, so they can read input and resume; Ui systems get the
  wall-clock frame time, and every phase has it as
  `SystemContext::unscaledDeltaSeconds`. The first-person camera ignores look
  input while the game is paused.
- `DebugDraw&`: `RenderRuntime::debugDraw()`, drawn in every view this frame.

Commands, timer callbacks, and event channels hold module code, so the
services are destroyed before the module that used them unloads.

## Folder contents

`src/engine/module/`: Game module ABI, shadow-copied or in-place loading and
unloading, module registration, the system registry and scheduler (Simulate
mask, per-system CPU profile), `Gameplay` (the component types and systems a
game plays) with `GameplayRunner`, the engine's built-in systems, and
`GameServices` (entity commands, events, timers).
