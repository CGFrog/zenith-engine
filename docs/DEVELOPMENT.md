# Development guide

## Prerequisites

- CMake 3.24 or newer
- Git (CMake fetches pinned dependencies during configuration)
- Python 3 (required by fetched build tooling)
- A C++20 compiler; MSVC is the documented Windows path
- A Vulkan 1.4-capable GPU and current driver for the executable
- `clang-format` 19 or newer for source formatting

CPU-side tests do not create a window or renderer, but configuring and building
still builds the engine and fetched dependencies.

## Configure and build

From the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 4
```

The first configuration downloads pinned dependencies into `build/_deps` and
needs network access. A successful build also:

- compiles recognized `shaders/` files to `build/shaders/*.spv`;
- copies `assets/` to `build/assets/`; and
- produces the executable under the generator's configuration directory (for
  Visual Studio, `build/Debug/zenith.exe`).

Useful variants:

```powershell
# Explicitly enable test targets
cmake -S . -B build -DBUILD_TESTING=ON

# Optimized build for benchmark measurements
cmake --build build --config Release --parallel 4
```

## Run and controls

```powershell
.\build\Debug\zenith.exe
```

| Input | Action |
| --- | --- |
| W / A / S / D | Move relative to view yaw. |
| Mouse | Look while captured. |
| Shift | Sprint. |
| Space | Jump while grounded. |
| R | Unload and recreate the gameplay scene. |
| Escape | Release mouse capture. |
| Mouse click | Recapture the mouse. |

## Tests

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Run one test by name:

```powershell
ctest --test-dir build -C Debug -R zenith_asset_scene --output-on-failure
ctest --test-dir build -C Debug -R zenith_physics --output-on-failure
ctest --test-dir build -C Debug -R zenith_application_lifecycle --output-on-failure
```

Tests are deterministic CPU-side contracts. Add one when changing importer
behavior, hierarchy semantics, component contracts, physics synchronization,
or failure handling. Keep GPU behavior in a bounded smoke test unless it
intrinsically requires a GPU-aware test harness.

## GPU smoke test

`ZENITH_SMOKE_FRAMES` makes the normal application exit after a fixed number of
presented frames:

```powershell
$env:ZENITH_SMOKE_FRAMES = 120
.\build\Debug\zenith.exe
Remove-Item Env:ZENITH_SMOKE_FRAMES
```

Use this after graphics, window, shader, or runtime-asset changes. A useful
manual pass also resizes, minimizes, and restores the window and checks for
validation errors. If the Khronos validation layer is absent, Zenith reports
that fact and continues.

## Rendering benchmark

Build Release and request a measured frame count:

```powershell
cmake --build build --config Release --parallel 4
$env:ZENITH_BENCHMARK_FRAMES = 20000
$env:ZENITH_BENCHMARK_OBJECTS = 2000 # optional deterministic render load
.\build\Release\zenith.exe
Remove-Item Env:ZENITH_BENCHMARK_FRAMES
Remove-Item Env:ZENITH_BENCHMARK_OBJECTS
```

Benchmark mode uses the fixed scene, disables gameplay and physics, warms up
for 1,000 frames, and prints JSON with median/p95 CPU and GPU frame time, draw
statistics, median FPS, and presentation mode. Compare results with the same
build type, object count, driver, GPU state, window size, and presentation mode.

## Formatting and verification

After editing C++:

```powershell
clang-format -i --style=file (rg --files src tests -g '*.cpp' -g '*.hpp')
```

Before handing off a normal code change:

```powershell
cmake --build build --config Debug --parallel 4
ctest --test-dir build -C Debug --output-on-failure
git diff --check
```

Also run the bounded smoke test for graphics/window changes. Documentation-only
changes need link/path review and `git diff --check`, but not a rebuild when
they do not alter generated inputs.

## Where to make a change

| Change | Primary location | Boundary to preserve |
| --- | --- | --- |
| Choose a game/scene | `src/main.cpp` and `src/game/` | Implement `Application`/`ApplicationScene`; do not teach `Engine` about content. |
| Compose demo entities | `src/game/scene/GameplayScene.*` | Keep asset names, controls, and gameplay outside `zenith_engine`. |
| Add generic ECS behavior | `src/engine/scene/` | Keep Vulkan and Jolt types out of general scene contracts. |
| Add a physical key | `src/engine/input/` | Expose device-independent state; interpret its gameplay meaning in the game layer. |
| Add an asset feature | `src/engine/assets/` | Produce CPU data; do not allocate GPU resources here. |
| Add physics behavior | `src/engine/physics/` | Hide Jolt behind Zenith types except deliberate native access. |
| Add a render pass | `src/engine/graphics/` and `shaders/` | Consume scene/assets without gameplay rules. |
| Add runtime content | `assets/` | Reference it from composition, not reusable subsystem code. |
| Change build wiring | `CMakeLists.txt` | Preserve generated shader/asset paths and dependencies. |

## Common tasks

### Add a renderable entity

Load a model, create a root, and instantiate it:

```cpp
const auto loaded = engine.assets().loadModel(modelPath);
if (!loaded) {
    return {.error = loaded.error};
}

const Entity root = engine.scene().createEntity("Ship");
return engine.scene().instantiateModel(root, loaded.handle, engine.assets());
```

Instantiation preserves the imported hierarchy and shares CPU/GPU mesh data
with every other instance of the same cached asset.

### Add a physical entity

Physical discovery requires transform, collider, and rigid-body components:

```cpp
const Entity crate = scene.createEntity("Crate");
scene.registry().emplace<ColliderComponent>(crate, BoxCollider{{0.5f, 0.5f, 0.5f}});

RigidBodyComponent body;
body.motionType = PhysicsMotionType::Dynamic;
body.layer = PhysicsLayer::Dynamic;
body.mass = 12.0f;
scene.registry().emplace<RigidBodyComponent>(crate, body);
```

The next physics step creates its body. After changing its components, call
`physics.refreshBody(crate, scene, assets)` and handle the returned error.

### Add a shader

Place GLSL under `shaders/` with a recognized stage extension such as `.vert`,
`.frag`, or `.comp`. Rebuild; CMake emits matching SPIR-V under
`build/shaders/`. Code loads it through `ZENITH_SHADER_DIR`. A new pipeline also
requires C++ pass/pipeline wiring and appropriate synchronization.

### Add an asset

Place content under `assets/` and rebuild `zenith_assets` (a normal build does
this automatically). Use `ZENITH_ASSET_DIR` when composing the runtime path.
Record third-party provenance and license information beside the asset.

### Add a system

Prefer a focused class that iterates only its required components. Pass
dependencies explicitly and invoke the system from composition/orchestration.
Add it to `zenith_engine` in `CMakeLists.txt` and test observable public behavior.

## Current limitations

- Scene loading is synchronous and supports one active scene; additive and
  asynchronous scene loading are not implemented.
- Scene changes retain the asset cache; explicit asset unloading is not
  implemented.
- Asset handles have a generation field, but append-only storage does not yet
  recycle slots.
- glTF animation, skins, morph targets, alpha modes, and compressed meshes are
  not rendered.
- Triangle-mesh colliders are static-only.
- Physics joints, vehicles, soft bodies, serialization, and debug drawing are
  not wrapped.
- Renderer and post-process settings are compiled into the implementation
  rather than exposed as public configuration.

Treat these as scope boundaries rather than reasons to bypass subsystem
ownership. Extend the narrow public contract and add tests when implementing
one of them.
