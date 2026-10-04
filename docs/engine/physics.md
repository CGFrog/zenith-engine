# Physics

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/physics/PhysicsComponents.hpp`

Defines static, kinematic, and dynamic motion; `PhysicsLayer` (an index into
the project's layer table); box, sphere, capsule, and triangle-mesh shapes; and
rigid-body tuning. The rigid body's `layer` field is schema version 2: any
index below `maxPhysicsLayers` (32), where version 1 allowed only the
built-in Static (0), Dynamic (1), and Player (2), which keep their indices. Primitive dimensions are multiplied by transform scale when the
Jolt shape is created. Triangle meshes reference a `MeshAsset` and are limited
to static bodies.

## `src/engine/physics/PhysicsWorld.*`

Owns Jolt through a private implementation. Each step discovers entities with
transform, collider, and rigid-body components; creates missing bodies; removes
orphaned bodies; moves kinematic bodies from ECS transforms; advances Jolt; and
writes dynamic translation/rotation back while preserving scale. Interpolation
blends previous and current dynamic poses for smooth rendering, writing each
body's world and local transforms and marking it dirty; it updates the scene's
transforms first only when an interpolated body has a parent. The host's
`updateWorldTransforms` after interpolation then carries the bodies' poses to
their children (`Engine::run` calls it once per frame).

It exposes forces, impulses, velocity, and begin/persist/end contacts in terms
of ECS entities, and these queries:

- `raycast` and `raycastAll` (every body along the ray, nearest first);
- `shapeCast` (a sphere, capsule, or box swept along a direction);
- `overlap` (every body a shape touches);
- `closestPoint` (the nearest body surface within a distance).

Each query takes a `PhysicsQueryFilter`: a layer mask, entities to ignore (a
ray's own shooter), and whether sensors count (by default they do not).

`triggerEvents()` lists the last step's sensor `Entered` and `Exited` events
per pair of bodies. They are derived on the main thread from Jolt's contacts.
- A body removed while inside (its entity destroyed, its body rebuilt) exits
  first. Each side carries its scene id, because the entity may already be
  gone.
- A body that falls asleep inside stays inside.
- `GameServices::publishTriggerEvents` turns them into `TriggerEntered` and
  `TriggerExited` events.

## `src/engine/physics/PhysicsLayers.*`

`PhysicsLayerTable` is a project's collision layers: up to 32 names, by
index, and a symmetric collision matrix. `defaults()` is Static, Dynamic, and
Player, where everything collides except two Static bodies. `PhysicsWorld`
starts with the defaults; hosts pass the project's table to `setLayers`, which
needs a world without bodies. A body names its layer by index; an index the
table lacks behaves as Dynamic. In Jolt, each body's object layer is its index
with a moving bit, so a layer may hold static and moving bodies. `refreshBody` rebuilds a body after collider or body
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
`JPH_DEBUG_RENDERER`, which `cmake/Dependencies.cmake` enables in every
configuration.
