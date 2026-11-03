# Gameplay API

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The gameplay API is the part of the engine a game module may use: the headers
it includes, the third-party types those headers expose, and the contexts
through which it reaches engine services. Code outside it (hosts, the renderer,
the editor) can change without notice to modules. Code inside it changes only
with a `ZENITH_MODULE_API_VERSION` bump (see
[Game modules](modules.md#systemcontext)).

`Zenith::Engine` still puts all of `src/` and the engine's third-party include
directories on a module's include path, so the boundary is a contract rather
than a wall. The build checks the engine's side of it (see
[Enforcement](#enforcement)).

## Supported headers

A module includes these headers, and the headers they include. The list lives
in `cmake/GameplayApi.cmake` (`ZENITH_GAMEPLAY_API_HEADERS`); the module
precompiled header (`engine/module/ZenithModulePch.hpp`) uses only these.

| Area | Headers (under `src/engine/`) |
| --- | --- |
| Module contract | `module/ModuleApi.hpp`, `module/ModuleRegistrar.hpp`, `module/EngineExport.hpp`, `module/SystemScheduler.hpp` (`SystemContext`, `SystemDescriptor`) |
| Gameplay services | `module/GameServices.hpp`, `module/GameServiceGroups.hpp`, `module/GameEvents.hpp`, `module/EventQueue.hpp`, `module/EntityCommandBuffer.hpp`, `module/Effects.hpp`, `module/Timers.hpp`, `module/SessionResources.hpp` |
| Behaviours | `behaviour/Behaviour.hpp` |
| Scene | `scene/Scene.hpp`, `scene/SceneEntityId.hpp`, `scene/ComponentTypes.hpp`, `scene/Tags.hpp`, `scene/Prefab.hpp` |
| Physics | `physics/PhysicsWorld.hpp`, `physics/PhysicsComponents.hpp`, `physics/CharacterController.hpp` |
| Gameplay mechanisms | `gameplay/FirstPerson.hpp`, `gameplay/Random.hpp`, `gameplay/DefinitionTable.hpp` |
| Assets | `assets/Assets.hpp`, `assets/MeshBuilder.hpp` |
| UI and input | `ui/GameUi.hpp`, `ui/GameMenus.hpp`, `input/InputActions.hpp`, `input/InputState.hpp` |
| Debug drawing | `graphics/DebugDraw.hpp` |

## Third-party types

Supported headers may expose only these in their declarations:

- the C++ standard library;
- glm, for vectors, matrices and quaternions;
- EnTT. `Entity` is `entt::entity`, and `Scene::registry()` returns the
  `entt::registry`, which modules use for components, views and per-scene
  context (`registry().ctx()`). EnTT is public by decision (the
  [roguelike readiness review](../ROGUELIKE_READINESS_REVIEW.md) rejected a
  wrapper as overengineering). Two rules from [Scene](scene.md#transforms-from-gameplay-code)
  apply: create and destroy entities through `Scene` (or
  `world().commands()`), never through `registry()`, and follow a raw write to
  `TransformComponent::local` with `markTransformDirty`;
- nlohmann::json, in component types' save and load functions.

SDL, Vulkan, VMA, volk, Jolt, Recast, cgltf, stb, ImGui and editor types are
not part of the gameplay API.

## Native escape hatch

`PhysicsWorld::nativeSystem()` and `nativeBodyInterface()` return Jolt's
`JPH::PhysicsSystem` and `JPH::BodyInterface` (forward-declared only). They
exist for physics the engine does not wrap yet. A module that uses them
includes and links Jolt itself, must match the engine's Jolt version and
build settings, and gets no stability promise: the module API version does
not cover Jolt's types. Ask for an engine mechanism instead when one would
serve more than one game.

## Host-only code

Modules must not include or use these, even though the include path reaches
them:

- `application/*` (`Application`, `ApplicationContext`, `ApplicationControl`)
  and `SystemContext::application`, which is for the engine's own systems.
  Use `SystemContext::services` (for example `services->content().assets()`
  for runtime meshes, materials and textures) and `SystemContext::physics`;
- `input/InputManager.hpp`, which translates SDL events (read `InputState` and
  `InputActionState` instead);
- `graphics/*` other than `DebugDraw.hpp`, and `window/*`;
- `module/Gameplay.hpp`, `module/GameModule.hpp` and the other loading and
  hosting files;
- everything under `src/editor/` and `src/game/`.

Editor extensions are a separate tier: a module's editor code includes
`editor/modules/EditorModuleApi.hpp` and may use ImGui (see
[Editor extensions](../EDITOR_EXTENSIONS.md)). Keep it apart from gameplay
code, which also runs in the packaged game.

## Rules

- A module reaches engine services only through the contexts it is handed:
  `SystemContext`, `BehaviourContext`, `GameServices`, and `ModuleRegistrar`
  at registration. No globals, no reaching into hosts.
- A change to the layout or signatures of a supported header bumps
  `ZENITH_MODULE_API_VERSION`.
- Adding a header to the gameplay API means adding it to
  `ZENITH_GAMEPLAY_API_HEADERS` and to the table above.
- A supported header that needs a host-only type forward-declares it or moves
  the type into a header of its own, as `InputState.hpp` was split from
  `InputManager.hpp`.

## Enforcement

`cmake/GameplayApi.cmake` builds `zenith_gameplay_api_check`, an object
library with one source per supported header containing just its `#include`.
It sees `src/` and EnTT, glm and nlohmann_json, and nothing else, so a
supported header that starts including SDL, Vulkan, Jolt, ImGui or another
private dependency fails the normal build.

The check covers the engine's headers, not what a module includes: a module
that includes host-only headers still compiles. Narrowing the include
directories `Zenith::Engine` exports to modules is later work.
