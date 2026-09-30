# Zenith Engine

Zenith is an educational C++20 game-engine project intended to grow into a
reusable foundation for future games.

## Documentation

- [`docs/REPOSITORY_GUIDE.md`](docs/REPOSITORY_GUIDE.md) explains every
  top-level directory, source module, runtime asset, shader, and test target.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) describes subsystem ownership,
  the ECS component model, startup, the frame loop, rendering, physics, and
  asset data flow.
- [`docs/EDITOR_TUTORIAL.md`](docs/EDITOR_TUTORIAL.md) is the minimal
  build-to-save level-authoring workflow.
- [`docs/tutorials/`](docs/tutorials/README.md) contains project guides,
  including a complete first-person target-range game walkthrough.
- [`docs/RENDERER_VALIDATION.md`](docs/RENDERER_VALIDATION.md) defines the
  performance, image-regression, and Vulkan smoke gates.
- [`docs/PERFORMANCE_ROADMAP.md`](docs/PERFORMANCE_ROADMAP.md) tracks the
  remaining measured CPU, GPU, graphics, physics, and streaming objectives.
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) covers configuration, building,
  testing, smoke tests, benchmarks, formatting, and common extension tasks.
- [`docs/RELEASING.md`](docs/RELEASING.md) covers Windows, Linux, and macOS
  artifacts, package contents, provenance, and clean-extraction verification.

## Build

The project uses Vulkan 1.4, Volk, GLM, SDL 3, cgltf, EnTT, Vulkan Memory
Allocator, Khronos glslang, and stb_image. Dependencies are pinned and downloaded
into the build tree by CMake `FetchContent`.

Requirements are CMake 3.24 or newer, a C++20 compiler, Git, Python 3, and a
Vulkan 1.4-capable GPU and driver. SDL 3 also supplies default-device audio;
the engine continues in null-output mode when no playback device is available.

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 4
```

GLSL files under `shaders/` compile automatically to matching SPIR-V files under
`build/shaders/`. Runtime assets are copied to `build/assets/`, so the executable
does not depend on its working directory.

## Run the gameplay-room template

```powershell
.\build\Debug\zenith.exe
```

The runtime opens on a controller-friendly title screen. Press Space, the
south gamepad button, or activate **Play** to enter the room. Escape/Start opens
the pause menu; keyboard, gamepad, and pointer navigation can resume play or
change antialiasing, bloom, audio mute, look sensitivity, and inverted-Y input.
Settings persist atomically in the platform user-data directory. The settings
screen also exposes high-contrast and reduced-motion modes; the underlying
accessibility profile includes scalable UI/text, captions, locale, and
color-independent indicator choices.

The application loads `assets/basic_map.gltf`, adds static collision to the
room, creates stacks of dynamic props, and spawns a physics-controlled player.
Move with WASD or the left stick, look with the mouse or right stick, jump with
Space or the south gamepad button, and hold Shift or press the left stick to
sprint. Press E or the west gamepad button to play the demo's original
synthesized positional interaction tone.
The demo also loops a quiet streamed Ogg background chord across scene reloads.
The player collides with the room and can push the props. Escape releases the
mouse; click the window to capture it again. Press R to unload and recreate the
gameplay scene. The Khronos validation layer is enabled when installed;
otherwise the application prints a notice and continues without it.

For an automated GPU smoke test, set `ZENITH_SMOKE_FRAMES` to a positive frame
count. The engine exits cleanly after presenting that many frames.

For a repeatable rendering benchmark, build `Release` and set
`ZENITH_BENCHMARK_FRAMES` to the number of measured frames. Full-runtime mode is
the default and advances gameplay, animation, physics, audio, and rendering with
a fixed simulation delta. Set `ZENITH_BENCHMARK_MODE=render` to freeze simulation
and isolate rendering. The harness warms up for 120 frames, then prints one JSON
report with median, p95, p99, maximum, and budget misses for each CPU/GPU scope.
Set `ZENITH_BENCHMARK_OBJECTS` to add a deterministic grid of repeated meshes:

```powershell
$env:ZENITH_BENCHMARK_FRAMES = 20000
$env:ZENITH_BENCHMARK_OBJECTS = 2000
.\build\Release\zenith.exe
```

`ZENITH_BENCHMARK_WARMUP_FRAMES`, `ZENITH_BENCHMARK_FRAME_BUDGET_MS`, and
`ZENITH_BENCHMARK_SAMPLES=1` control warmup, the whole-frame budget, and optional
raw sample output. The manual `zenith_render_packet_benchmark` and
`zenith_simulation_benchmark` executables cover CPU packet caching and sleeping
versus active rigid-body workloads without making unstable timing thresholds part
of the test suite.

## Assets and scene components

`AssetManager` parses and caches `.gltf` and `.glb` files. CPU model, mesh, and
material data use opaque generational handles; Vulkan allocations remain in the
renderer-owned GPU cache, so many entities can share a single mesh upload.
Models expand into an EnTT hierarchy: the requested root receives
`ModelComponent`, imported nodes receive `TransformComponent`, and drawable
nodes receive `MeshRendererComponent`.

Morph targets and animation weight channels import with a deterministic active
target bound. KTX2 RGBA8 content has a CPU fallback, while optional compressed
mesh and Basis decoders report their exact missing capability. Deterministic
derived data is stored outside source folders and can be built with
`zenith_asset_build` or inspected and invalidated by editor tooling.

The `zenith_demo` application library owns the map, player camera, controller,
gameplay action mapping, menus, and settings policy. `Engine` initializes
reusable systems and hosts an `ApplicationSession` without naming any scene.
SDL event polling and mouse capture stay in `InputManager`; demo code consumes
device-independent actions. Runtime menus and HUDs record engine game-UI
commands and do not include ImGui or Vulkan headers.

## Physics

Zenith wraps Jolt Physics 5.6 behind ECS components in
`engine/physics/PhysicsComponents.hpp`. The simulation runs at a fixed 60 Hz by
default, independently of rendering. `PhysicsWorldConfig` controls gravity,
step rate, catch-up limits, capacities, worker threads, and collision layers.

Add a transform, collider, and rigid body to make an entity physical:

```cpp
Entity crate = scene.createEntity("Crate");
scene.registry().emplace<ColliderComponent>(crate, BoxCollider{{0.5f, 0.5f, 0.5f}});
scene.registry().emplace<RigidBodyComponent>(crate, RigidBodyComponent{
    .motionType = PhysicsMotionType::Dynamic,
    .layer = PhysicsLayer::Dynamic,
    .mass = 12.0f,
    .friction = 0.7f,
});
```

Box, sphere, capsule, and static triangle-mesh colliders are supported. A
transform's scale is baked into its collision shape; dynamic translation and
rotation are written back without losing that visual scale. After changing an
existing body's collider or creation settings, call
`engine.physics().refreshBody(entity, scene, assets)`. Forces and gameplay can
use forces, impulses, velocity access, raycasts (including hit entity, point, and
normal), and per-step begin/persist/end contact events.

The default `Static`, `Dynamic`, and `Player` layer definitions can be replaced
through `PhysicsWorldConfig::layers`. Most code should use Zenith's types, while
advanced integrations can deliberately opt into `nativeSystem()` or
`nativeBodyInterface()`.

`DemoApplication` is the current game client. Its `GameplayScene` combines a
glTF room with code-authored physics, player configuration, and props. Scene
changes are deferred to a frame boundary; unloading clears ECS and physics
state while retaining cached assets and renderer resources. Joints, vehicles,
soft bodies, serialization, and debug drawing are intentionally left for later
layers. Triangle meshes are static-only; use primitives for moving objects.

Versioned prefab documents provide stable IDs, nested composition, separate
property/component overrides, transactional refresh, and project-relative
asset references. The editor authoring service makes create, instantiate,
override, unpack, apply, and revert operations undoable; the runtime prefab
system is also usable in additive scene instances.

## Audio

`AudioSystem` owns SDL device output, decoding, mixing, and voice lifetime. WAV
and Ogg effects are cached by canonical path as immutable float PCM; Ogg music
is decoded on a worker into a bounded ring buffer. Public APIs use generational
clip, stream, and voice handles and expose fixed master, music, and SFX buses.

`AudioListenerComponent` and `AudioEmitterComponent` are declarative ECS state.
Callers keep runtime playback state out of components; the engine synchronizes
world transforms after physics interpolation. Scene transitions stop scoped
voices before clearing entities, while persistent music survives until stopped
or engine shutdown. Spatial audio uses Zenith's +Y-up, -Z-forward, metre-based
coordinates, inverse-distance attenuation, and equal-power stereo panning.

## Code style

The C++ sources use the repository's `.clang-format` configuration. Control
statements always use braces, related operations are separated into readable
blocks, and implementation details stay in their owning subsystem. To format
the source and tests with clang-format 19 or newer:

```powershell
clang-format -i --style=file (rg --files src tests -g '*.cpp' -g '*.hpp')
```

```cpp
auto loaded = assets.loadModel("models/ship.glb");
if (loaded) {
    Entity ship = scene.createEntity("Ship");
    scene.instantiateModel(ship, loaded.handle, assets);
}
```

The importer supports triangle primitives, indexed and non-indexed geometry,
node transforms, skin weights, animation clips, embedded/external buffers and
images, and glTF metallic-roughness PBR materials. Opaque, masked, blended, and
double-sided materials are rendered; masked surfaces also cast cutout shadows.
Missing normals and tangents are generated. Morph targets and compressed mesh
extensions are not yet rendered. Unsupported non-triangle primitives are
skipped; malformed assets and models without renderable primitives return
descriptive errors.

The renderer uses dynamic rendering, synchronization2, staging uploads,
mipmapped anisotropic textures, per-frame camera buffers, push constants,
VMA-backed vertex/index buffers, and three frames in flight. The mesh pass uses
Cook-Torrance GGX lighting, bounded tiled point/spot lighting, cached
roughness-prefiltered environments, deterministic reflection-probe blending,
and a stabilized 2,048-pixel directional shadow map with PCF. Project settings
select validated quality fallbacks and per-pass CPU/GPU budgets. A single fused
HDR post-process pass adds
thresholded bloom, ACES tone mapping, and edge-adaptive anti-aliasing.

## Tests

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

The CPU-side tests cover audio decoding, caching, mixing, buses, handle and
voice lifetime, spatialization, model loading, path caching, generated normals,
materials, component instantiation, hierarchy cleanup, rigid-body settling,
contacts, impulses, raycasts, static mesh collision, and entity cleanup without
requiring a GPU. For a visual smoke test, resize, minimize, and restore the
window; the room and physics props should return without validation errors.
