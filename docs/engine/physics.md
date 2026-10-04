# Physics

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/physics/PhysicsComponents.hpp`

Defines static, kinematic, and dynamic motion; collision layers; box, sphere,
capsule, and triangle-mesh shapes; rigid-body tuning; and first-person character
tuning/state. Primitive dimensions are multiplied by transform scale when the
Jolt shape is created. Triangle meshes reference a `MeshAsset` and are limited
to static bodies.

## `src/engine/physics/PhysicsWorld.*`

Owns Jolt through a private implementation. Each step discovers entities with
transform, collider, and rigid-body components; creates missing bodies; removes
orphaned bodies; moves kinematic bodies from ECS transforms; advances Jolt; and
writes dynamic translation/rotation back while preserving scale. Interpolation
blends previous and current dynamic poses for smooth rendering.

It exposes forces, impulses, velocity, raycasts, and begin/persist/end contacts
in terms of ECS entities. `refreshBody` rebuilds a body after collider or body
settings change. Native Jolt accessors are deliberate advanced escape hatches.
`drawDebug` records collider wireframes and other body diagnostics into a
`DebugDraw`.

## `src/engine/physics/JoltRuntime.*`

Private reference-counted owner of Jolt's process-wide state (factory,
registered types, the one debug renderer). Each initialized `PhysicsWorld`
holds a move-only `JoltRuntimeLease`; worlds coexist and shut down in any order.

## `src/engine/physics/JoltDebugDraw.*`

Private adapter from Jolt's `DebugRendererSimple` to `DebugDraw`: lines and
triangle edges become debug lines, 3D text becomes labels. It needs Jolt's
`JPH_DEBUG_RENDERER`, which the root `CMakeLists.txt` enables in every
configuration.
