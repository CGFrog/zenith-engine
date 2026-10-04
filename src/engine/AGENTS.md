# Engine workspace guide

This subtree is the reusable `zenith_engine` library. It must not include from
`editor/`, `game/`, or an executable entry point.

## Directory map

| Path | Responsibility |
| --- | --- |
| `Engine.*` | Top-level ownership and runtime loop orchestration. [Application](../../docs/engine/application.md) |
| `animation/` | Clip sampling, pose blending, animation playback, and the animator state machine. [Animation](../../docs/engine/animation.md) |
| `application/` | Application/scene lifecycle contracts, deferred requests, and the simulation clock. [Application](../../docs/engine/application.md) |
| `assets/` | Renderer-independent import, decoding, caching, and typed asset handles. [Assets](../../docs/engine/assets.md) |
| `audio/` | SDL audio device, decoding, mixing, streaming, and audio handles. [Audio](../../docs/engine/audio.md) |
| `behaviour/` | The `Behaviour` base class, its registry, and the scheduler that runs behaviour hooks. [Modules](../../docs/engine/modules.md) |
| `diagnostics/` | The engine `Logger`. [Application](../../docs/engine/application.md) |
| `gameplay/` | Reusable gameplay mechanisms: first-person players (driven by the built-in systems), seeded random streams, and definition tables. [Gameplay](../../docs/engine/gameplay.md) |
| `graphics/` | Renderer and Vulkan implementation. [Graphics](../../docs/engine/graphics.md), [passes](../../docs/engine/graphics-passes.md), [post](../../docs/engine/graphics-post.md) |
| `input/` | Device-state translation; gameplay meaning belongs in `src/game/`. [Window and input](../../docs/engine/window-input.md) |
| `jobs/` | Bounded prioritized background work. [Scene](../../docs/engine/scene.md) |
| `module/` | Game module ABI, loading, system registry and scheduler, and `Gameplay`. [Modules](../../docs/engine/modules.md) |
| `navigation/` | Recast/Detour navigation meshes behind `NavigationWorld`, and agents that follow paths. [Navigation](../../docs/engine/navigation.md) |
| `physics/` | Jolt-backed simulation exposed through Zenith domain types. [Physics](../../docs/engine/physics.md) |
| `platform/` | OS services: user-data locations, atomic writes, diagnostics bundles, and child processes. [Platform](../../docs/engine/platform.md) |
| `project/` | The shared versioned project manifest and project scene loading. [Projects](../../docs/engine/projects.md) |
| `reflection/` | Plain-data field descriptors and their JSON codec; no scene or asset knowledge. [Scene](../../docs/engine/scene.md) |
| `scene/` | ECS components, hierarchy, collections, prefabs, component registry, serialization. [Scene](../../docs/engine/scene.md) |
| `ui/` | Resolution-independent runtime menus and HUDs (`GameUi`). [UI](../../docs/engine/ui.md) |
| `window/` | SDL window ownership and platform surface access. [Window and input](../../docs/engine/window-input.md) |

## Local rules

- Keep asset parsing independent of GPU ownership and physics independent of
  rendering.
- Keep third-party types private unless native access is an intentional part of
  the public API.
- Put a new file in the narrowest existing subsystem and add it to
  `zenith_engine` in `src/engine/CMakeLists.txt`.
- A serialized engine component's type is built in the `scene/` family file
  for its domain (`RenderComponentTypes.cpp`, `AnimationComponentTypes.cpp`,
  `LightComponentTypes.cpp`, `PhysicsComponentTypes.cpp`; collection and
  visibility components in `CollectionComponentTypes.cpp`, prefab link
  components in `PrefabComponentTypes.cpp`) and listed in
  `scene/BuiltInComponentTypes.cpp`, which fixes the registration order; append
  new types rather than reordering. Codec helpers shared by the families live
  in the internal `ComponentTypeHelpers.hpp`.
  `SceneDocument` and the editor inspector discover it from the registry.
  Changing a field key or meaning needs a schema version and a migration.
- `zenith_engine` is a shared library. Functions are exported automatically;
  exported data needs `ZENITH_ENGINE_API`. Do not call privately linked
  third-party libraries (volk, Jolt, shaderc) from inline header code.
- Prefer CPU-only coverage under the matching `tests/engine/` area. Graphics or
  window work also needs a bounded smoke test when Vulkan is available.
