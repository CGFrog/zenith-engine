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
`SystemRegistry`, recording which component keys the module added.
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
types alone. `GameplayRunner` plays a `Gameplay` in a game's
`ApplicationScene` (the project application and the demo use it).
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
- `actions`: the game's evaluated input actions, or null when the host
  evaluates none.

Changing `SystemContext`, `ModuleRegistrar`, or a registered descriptor bumps
`ZENITH_MODULE_API_VERSION` (now 3; A3 changed `RigidBodyComponent::layer`).

## `GameServices`

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
- `Timers`: one-shot or repeating callbacks on simulated time. They follow the
  clock's time scale and stop while paused. A timer may have an owner entity;
  the timer is dropped when its owner is gone.
- `DebugDraw&`: `RenderRuntime::debugDraw()`, drawn in every view this frame.

Commands, timer callbacks, and event channels hold module code, so the
services are destroyed before the module that used them unloads.

## Folder contents

`src/engine/module/`: Game module ABI, shadow-copied or in-place loading and
unloading, module registration, the system registry and scheduler (Simulate
mask, per-system CPU profile), `Gameplay` (the component types and systems a
game plays) with `GameplayRunner`, and `GameServices` (entity commands,
events, timers).
