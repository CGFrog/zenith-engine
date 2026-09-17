# Zenith Engine

Zenith is an educational C++20 game-engine project intended to grow into a
reusable foundation for future games.

## Build

The project uses Vulkan 1.4, Volk, GLM, SDL 3, cgltf, EnTT, Vulkan Memory
Allocator, and Khronos glslang. Dependencies are pinned and downloaded into the
build tree by CMake `FetchContent`.

Requirements are CMake 3.24 or newer, a C++20 compiler, Git, Python 3, and a
Vulkan 1.4-capable GPU and driver.

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

The application loads `assets/basic_map.gltf`, adds static collision to the
room, creates stacks of dynamic props, and spawns a physics-controlled player.
Move with WASD, look with the mouse, jump with Space, and hold Shift to sprint.
The player collides with the room and can push the props. Escape releases the
mouse; click the window to capture it again. The Khronos validation layer is
enabled when installed; otherwise the application prints a notice and
continues without it.

For an automated GPU smoke test, set `ZENITH_SMOKE_FRAMES` to a positive frame
count. The engine exits cleanly after presenting that many frames.

## Assets and scene components

`AssetManager` parses and caches `.gltf` and `.glb` files. CPU model, mesh, and
material data use opaque generational handles; Vulkan allocations remain in the
renderer-owned GPU cache, so many entities can share a single mesh upload.
Models expand into an EnTT hierarchy: the requested root receives
`ModelComponent`, imported nodes receive `TransformComponent`, and drawable
nodes receive `MeshRendererComponent`.

The executable composes its map, player camera, and controller in `main.cpp`.
`Engine` initializes and runs reusable systems without naming or loading any
particular scene, and the renderer only consumes camera and mesh components.
SDL event polling, keyboard/mouse mapping, and mouse capture are isolated in
`InputManager`; the engine loop consumes a device-independent `InputState`, and
the first-person controller translates that state into physics velocity.

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

`createTemplateScene` is the reusable starting point for a game: it combines a
glTF room with code-authored physics, player configuration, and props. Joints,
vehicles, soft bodies, serialization, and debug drawing are intentionally left
for later layers. Triangle meshes are static-only; use primitives for moving
objects.

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

The current importer supports static opaque triangle primitives, indexed and
non-indexed geometry, node transforms, positions, normals, embedded/external
buffers, and base-color factors. Missing normals are generated. Textures, alpha
modes, animation, skins, morph targets, and compressed mesh extensions are not
yet rendered. Unsupported non-triangle primitives are skipped; malformed assets
and models without renderable primitives return descriptive errors.

The mesh pass uses dynamic rendering, synchronization2, staging uploads,
per-frame camera buffers, push constants, VMA-backed vertex/index buffers, a
depth attachment, and three frames in flight.

## Tests

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

The CPU-side tests cover model loading, path caching, generated normals,
materials, component instantiation, hierarchy cleanup, rigid-body settling,
contacts, impulses, raycasts, static mesh collision, and entity cleanup without
requiring a GPU. For a visual smoke test, resize, minimize, and restore the
window; the room and physics props should return without validation errors.
