# Development guide

## Prerequisites

- CMake 3.25 or newer
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

`cmake --preset default` is the same configuration. In a git worktree,
`cmake --preset worktree` with `$env:ZENITH_SHARED_DEPS_DIR` set to a configured
checkout's `build/_deps` reuses that checkout's dependency sources
(`ZENITH_SHARED_DEPS_DIR` in `cmake/Dependencies.cmake`) without network access,
and `tools/check.ps1` builds, runs the CPU-only tests, and formats the changed
C++ files.

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

### Build speed

Three cache options (`cmake/CompilerSettings.cmake`) shorten builds; set them
with `-D<option>=<value>` when configuring.

| Option | Default | Effect |
| --- | --- | --- |
| `ZENITH_USE_PCH` | `ON` (`OFF` when an MSVC compiler cache is in use) | Precompiles `src/engine/ZenithEnginePch.hpp` for `zenith_engine` and `src/editor/ZenithEditorPch.hpp` for `zenith_editor_app`. |
| `ZENITH_VS_PARALLEL_COMPILE` | `ON` | With a Visual Studio generator, compiles each project's sources in parallel (MSBuild `UseMultiToolTask`), capped at one compiler per core across the projects `--parallel` builds at once. |
| `ZENITH_COMPILER_CACHE` | `auto` | `auto` launches compilers through `sccache`, else `ccache`, when one is on `PATH`; `sccache` or `ccache` requires it; `off` disables caching. |

The precompiled headers hold only standard and third-party headers that are
not macro sensitive (no Vulkan, volk, SDL, Jolt, or Windows headers, and no
Zenith headers). Sources never rely on them: every file includes what it uses,
and a tree configured with `-DZENITH_USE_PCH=OFF` must still build. Check that
after changing includes widely. `zenith_engine` exports every symbol, so it
reuses a header compiled in the `zenith_engine_pch` static library rather than
linking the header's own object file, whose symbols would otherwise be
exported.

A compiler cache helps most for new worktrees and branch switches. Install
`sccache` once with `winget install Mozilla.sccache`. CMake runs a compiler
launcher only with the Ninja and Makefile generators, so the default Visual
Studio generator builds uncached; configure a separate tree with `-G Ninja`
from a Visual Studio developer prompt to use it. A cached MSVC build embeds
debug information in each object (`/Z7`) instead of a shared PDB. MSVC caches
cannot cache translation units that use a precompiled header, which is why
the precompiled headers default to off with a cache; with both on, the cache
covers only the third-party and test sources.

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

[Testing](TESTING.md) is the rule book: the tiers, how to write a test, and
how to run one. In short:

```powershell
ctest --preset unit    # the everyday loop: CPU-only unit tests
ctest --preset cpu     # unit and slow: what CI runs; before pushing
ctest --preset gpu     # smokes, validation, captures (needs a Vulkan device)
ctest --preset all     # everything
```

GPU tests set `PROCESSORS 2`, so `-j 8` runs at most four engine instances on
the GPU at once.

On a much slower machine, such as Linux CI running the Vulkan tests on Mesa's
lavapipe (a CPU Vulkan driver) under Xvfb, configure with
`-DZENITH_TEST_TIMEOUT_SCALE=4` to multiply every test's timeout. Validation
needs a Vulkan 1.4 aware layer: Ubuntu 24.04's packaged one (1.3.275) crashes
on core push descriptors, so CI takes `VkLayer_khronos_validation` from the
LunarG SDK and points `VK_ADD_LAYER_PATH` at it. Reference captures skip
themselves without a hardware device.

Run one test or case by name:

```powershell
ctest --preset unit -R "engine.GameServices"            # a file's cases
build/Debug/zenith_engine_tests -tc="GameServices: timers"  # one case, directly
ctest --test-dir build -C Debug -R zenith_asset_scene    # a standalone test
```

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
isolate render submission and GPU work. Delayed GPU results retain their
original submission identity. The implementation roadmap and measurement
contract live in [Performance roadmap](PERFORMANCE_ROADMAP.md).

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

`zenith_secondary_scene_benchmark` (Vulkan) measures secondary render scenes,
the editor's prefab windows: 0, 1, 2, 4, and 8 scenes of
`ZENITH_SECONDARY_BENCHMARK_OBJECTS` cubes (500), each static, with one object
moving, and bound but hidden, over `ZENITH_SECONDARY_BENCHMARK_WARMUP` (10) and
`ZENITH_SECONDARY_BENCHMARK_FRAMES` (120) frames per case. It samples the
renderer's CPU time without fence wait, acquire, and present, the part spent on
secondary scenes, and their packet rebuilds.

CTest also runs both (label `benchmark`) to check that they complete and exit
cleanly; read timings from a Release build, not from those runs.

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
ctest --test-dir build -C Debug --output-on-failure -j 8
git diff --check
```

Also run the bounded smoke test for graphics/window changes. Documentation-only
changes need link/path review and `git diff --check`, but not a rebuild when
they do not alter generated inputs.

### Game module iteration

What changing gameplay code costs in the editor is the project's module build
plus a hot reload. `zenith_generated_module_runs --measure-iteration [folder]`
(`zenith_module_iteration_benchmark_run`, label `benchmark`)
generates an FPS project in the build tree and times the editor's own build
request (`projectBuildRequest`, through `BuildRunner`):
- the first build;
- a build with nothing changed;
- one with a source edited;
- one with a header that all five sources include edited;
- loading the module.

The project is not under the temporary directory, because MSBuild's file
tracker ignores reads from it, so a header edit there rebuilds nothing. The
editor reports each build's time in the Build diagnostics ("Built in 1.2 s")
and each reload's phases in the Game Module ones.

Debug, seconds:

| Measure | Baseline | Module PCH | Fast build (Ninja) |
| --- | --- | --- | --- |
| First build | 25.30 | 10.26 | 7.53 |
| Nothing changed | 1.17 | 1.20 | 0.12 |
| One source edited | 5.75 | 2.61 | 1.36 |
| Shared header edited | 19.89 | 2.67 | 1.57 |
| Module load (shadow copy, register) | 0.16 | 0.11 | 0.11 |

- **Baseline:** the build configures every time, uses the Visual Studio
  generator, and has no module PCH.
- **Module PCH:** `ZenithEngineConfig.cmake` gives `Zenith::Engine` these
  usage requirements, so a project gets them the next time it configures,
  with no edits:
  - `INTERFACE_PRECOMPILE_HEADERS` set to
    `src/engine/module/ZenithModulePch.hpp`, which holds the engine's
    gameplay headers and the standard and third-party ones they use.
    `-DZENITH_MODULE_PCH=OFF` turns it off, and every module source must
    still build without it.
  - `/DEBUG:FASTLINK` for Debug.

  `zenith_configure_game_module(<target>)`, which the templates call, adds
  parallel compilation under the Visual Studio generator.
- **Fast build:** the editor's own build of the project
  (`editor/project/FastProjectBuild`). `--measure-iteration --preset`
  measures the preset build instead.
  - It is a Ninja tree in `<project>/.zenith/build`, configured only while
    its `build.ninja` is missing. Ninja reconfigures itself when a
    `CMakeLists.txt` or the engine's config changes.
  - It runs with the engine's compiler, through the developer environment
    of the engine's Visual Studio (`editor/project/DeveloperEnvironment`).
    That environment is `vcvars64.bat`'s, captured in the background when
    a project opens and cached in the editor's settings directory.
  - The project's own preset build tree (`build/`) stays for its IDE. Both
    copy the module to `modules/`.

## Game projects outside the repository

The editor's Project Hub generates C++ game projects from
`templates/cpp_game/` (see the [user guide](guide/01-getting-started.md)). A
generated project builds its game module against a configured and built
Zenith build directory, which configuration writes two files into:

- `ZenithEngineConfig.cmake` (with `ZenithEngineTargets-<config>.cmake`)
  defines the imported `Zenith::Engine` target: the engine's include
  directories and definitions and, per configuration, the engine and SDL
  libraries. The project's `CMakeLists.txt` includes it from `ZENITH_ROOT`
  and refuses a compiler other than the engine's.
- `zenith-engine.json` records the CMake generator, generator instance, and
  compiler; the generator repeats them in the project's `CMakePresets.json`
  (the `zenith` preset), because a module only loads into an engine built
  with the same compiler and runtime (the module build hash).

The editor finds templates in `<resource root>/templates` (the build copies
`templates/` there) and then in the source tree's `templates/`, and uses the
resource root as `ZENITH_ROOT` when it is a Zenith build directory, else the
build it was compiled in. New projects build the editor's own configuration.
`zenith_project_template_module_build` (label `slow`) generates a project and
builds and loads its module; pass a folder to keep the result:
`build\Debug\zenith_generated_module_runs.exe --build-generated-module <folder>`.

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
| Change build wiring | `CMakeLists.txt`, the nearest folder `CMakeLists.txt`, `cmake/` | Preserve generated shader/asset paths and dependencies. |

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
.\build\Debug\zenith_asset_build.exe cook-model-meshes assets\model.gltf build\derived
```

Delete `build/derived` to exercise the RGBA8 fallback path (meshes are then
cooked again on their first load).

Never place derived files beside source assets. Importers must
validate `AssetImportLimits` before allocating decoded buffers; optional
Meshopt, Draco, or Basis paths use the decoder boundary and retain the named
missing-decoder error.

### Add a system

Prefer a focused class that iterates only its required components. Pass
dependencies explicitly and invoke the system from composition/orchestration.
Add it to `zenith_engine` in `src/engine/CMakeLists.txt` and test observable public behavior.

## Current limitations

- Scene loading is synchronous in the application session; the additive loader
  separates background file reads and parsing from main-thread scene commits.
- Asset handles identify recyclable generational slots. Live scenes, renderer
  frames, editor operations, and audio voices retain leases; deterministic
  collection reclaims unused generations according to configured budgets.
- Morph targets use a bounded active-target path. Optional compressed-asset
  decoders remain capability-gated and diagnose unavailable codecs.
- Triangle-mesh colliders are static-only; convex-hull colliders work on any body.
- Physics joints, vehicles, soft bodies, serialization, and debug drawing are
  not wrapped.
- Platform packaging beyond the host OS is verified by its corresponding CI
  runner rather than cross-built locally.

Treat these as scope boundaries rather than reasons to bypass subsystem
ownership. Extend the narrow public contract and add tests when implementing
one of them.
