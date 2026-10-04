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
`SystemScheduler` runs a registry's systems per `SystemPhase` (update, fixed
update) by ascending order, filtered by `RunIn` edit/play flags; in
`SimulationMode::Simulate` it runs the Play systems its `SimulateSystemMask`
enables (default: FixedUpdate on, Update off). An optional `SystemProfile`
passed to `run` records per-system CPU time (frame, rolling average, maximum). Everything a
module registered must be released, and every scene its components touched
destroyed, before the `GameModule` is destroyed; otherwise the library is
deliberately left loaded. `Gameplay.hpp` is what a game plays: a component
type registry and a `SystemScheduler`, from a module (`gameplayFromModule`),
from registration linked into the executable (`linkGameplay`), or the built-in
types alone; `Gameplay::play` runs its Play systems for a phase with the
application's services and the frame's input. `EngineExport.hpp` defines
`ZENITH_ENGINE_API` for data that must cross the engine library boundary.

## Folder contents

`src/engine/module/`: Game module ABI, shadow-copied or in-place loading and
unloading, module registration, the system registry and scheduler (Simulate
mask, per-system CPU profile), and `Gameplay` (the component types and systems
a game plays).
