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
$env:ZENITH_BENCHMARK_MODE = 'full'  # full (default) or render
$env:ZENITH_BENCHMARK_WARMUP_FRAMES = 120
$env:ZENITH_BENCHMARK_FRAME_BUDGET_MS = 8.333
.\build\Release\zenith.exe
Remove-Item Env:ZENITH_BENCHMARK_FRAMES
Remove-Item Env:ZENITH_BENCHMARK_OBJECTS
Remove-Item Env:ZENITH_BENCHMARK_MODE
Remove-Item Env:ZENITH_BENCHMARK_WARMUP_FRAMES
Remove-Item Env:ZENITH_BENCHMARK_FRAME_BUDGET_MS
```

Benchmark mode uses the fixed indoor/outdoor scene, adds 96 deterministic local
point/spot lights when `ZENITH_BENCHMARK_OBJECTS` is nonzero, and warms up before
measurement. `full` advances gameplay, animation, physics, and audio with one
fixed simulation interval per rendered frame; `render` freezes those systems to
isolate render submission and GPU work.

The schema-v2 JSON contains build revision/dirty state, compiler, platform, GPU,
resolution, presentation mode, workload settings, memory totals, and sample
summaries (`count`, `mean`, `median`, `p95`, `p99`, `max`, `budget`, and
`over_budget`). CPU scopes include input, application update, animation, physics,
transforms, UI construction, renderer, audio, frame-slot wait, image acquire, and
present. GPU samples are tagged with their original submission IDs so delayed
query results are neither attributed to the wrong CPU frame nor counted twice.
The final in-flight submissions may not retire before exit, so GPU and CPU sample
counts can differ. Set `ZENITH_BENCHMARK_SAMPLES=1` only when raw samples are
needed for spike correlation.

Two CPU-only/manual workload executables avoid display pacing when investigating
specific systems:

```powershell
.\build\Release\zenith_render_packet_benchmark.exe
.\build\Release\zenith_simulation_benchmark.exe
```

The packet benchmark reports rebuild/sort and cached-revision distributions after
warmup. The simulation benchmark reports separate 256-body sleeping and active
Jolt workloads, including sync phases, solver time, contacts, and active/sleeping
body counts. Neither executable uses machine-dependent timing as a test pass/fail
threshold. Compare reports only with the same build, scene/object count, driver,
GPU and power state, window size, and presentation mode.

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
`build/shaders/`. Code resolves it with `shaderRoot()` from
`engine/resources/ResourceLocator.hpp`. A new pipeline also requires C++
pass/pipeline wiring and appropriate synchronization.

### Add an asset

Place content under `assets/` and rebuild `zenith_assets` (a normal build does
this automatically). Use `assetRoot()` when composing a runtime path. Record
third-party provenance and license information beside the asset.

For deterministic derived data, run:

```powershell
.\build\Debug\zenith_asset_build.exe assets\model.gltf .zenith\derived `
  "quality=production" "zenith-ddc-1" "windows-x64"
```

The key includes source bytes, the exact settings string, tool version, and
target platform.

Block-compressed textures use the same cache. The build already cooks every
`assets/*.gltf` into `build/derived`; to cook other content run:

```powershell
.\build\Debug\zenith_asset_build.exe cook-model-textures assets\model.gltf build\derived
.\build\Debug\zenith_asset_build.exe cook-texture assets\textures\foo.png build\derived normal
```

Delete `build/derived` to exercise the RGBA8 fallback path.

Never place derived files beside source assets. Importers must
validate `AssetImportLimits` before allocating decoded buffers; optional
Meshopt, Draco, or Basis paths use the decoder boundary and retain the named
missing-decoder error.

### Add a system

Prefer a focused class that iterates only its required components. Pass
dependencies explicitly and invoke the system from composition/orchestration.
Add it to `zenith_engine` in `CMakeLists.txt` and test observable public behavior.

## Current limitations

- Scene loading is synchronous in the application session; the additive loader
  separates background file reads and parsing from main-thread scene commits.
- Asset handles identify recyclable generational slots. Live scenes, renderer
  frames, editor operations, and audio voices retain leases; deterministic
  collection reclaims unused generations according to configured budgets.
- Morph targets use a bounded active-target path. Optional compressed-asset
  decoders remain capability-gated and diagnose unavailable codecs.
- Triangle-mesh colliders are static-only.
- Physics joints, vehicles, soft bodies, serialization, and debug drawing are
  not wrapped.
- Platform packaging beyond the host OS is verified by its corresponding CI
  runner rather than cross-built locally.

Treat these as scope boundaries rather than reasons to bypass subsystem
ownership. Extend the narrow public contract and add tests when implementing
one of them.
