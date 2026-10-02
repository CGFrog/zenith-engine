# Engine workspace guide

This subtree is the reusable `zenith_engine` library. It must not include from
`editor/`, `game/`, or an executable entry point.

## Directory map

| Path | Responsibility |
| --- | --- |
| `Engine.*` | Top-level ownership and runtime loop orchestration. |
| `application/` | Application/scene lifecycle contracts and deferred control requests. |
| `assets/` | Renderer-independent import, decoding, caching, and typed asset handles. |
| `audio/` | SDL audio device, decoding, mixing, streaming, and audio handles. |
| `graphics/` | Renderer and Vulkan implementation; nested folders separate passes, resources, pipelines, surfaces, swapchains, shaders, and Vulkan setup. |
| `input/` | Device-state translation; gameplay meaning belongs in `src/game/`. |
| `physics/` | Jolt-backed simulation exposed through Zenith domain types. |
| `platform/` | OS services: user-data locations, atomic writes, diagnostics bundles, and child processes. |
| `reflection/` | Plain-data field descriptors and their JSON codec; no scene or asset knowledge. |
| `scene/` | ECS components, hierarchy, transforms, the component type registry, and scene serialization. |
| `window/` | SDL window ownership and platform surface access. |

## Local rules

- Keep asset parsing independent of GPU ownership and physics independent of
  rendering.
- Keep third-party types private unless native access is an intentional part of
  the public API.
- Put a new file in the narrowest existing subsystem and add it to
  `zenith_engine` in the root `CMakeLists.txt`.
- A serialized component is registered in `scene/BuiltInComponentTypes.cpp`;
  `SceneDocument` and the editor inspector discover it from the registry.
  Changing a field key or meaning needs a schema version and a migration.
- Prefer CPU-only coverage under the matching `tests/engine/` area. Graphics or
  window work also needs a bounded smoke test when Vulkan is available.
