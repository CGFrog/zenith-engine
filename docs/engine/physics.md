# Physics

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/physics/PhysicsComponents.hpp`

Defines static, kinematic, and dynamic motion; `PhysicsLayer` (an index into
the project's layer table); box, sphere, capsule, and triangle-mesh shapes; and
rigid-body tuning. The rigid body's `layer` field is schema version 2: any
index below `maxPhysicsLayers` (32), where version 1 allowed only the
built-in Static (0), Dynamic (1), and Player (2), which keep their indices. Primitive dimensions are multiplied by transform scale when the
Jolt shape is created. Triangle meshes reference a `MeshAsset`, imported or
created at runtime (`AssetManager::createMesh`), and are limited to static
bodies.

A collider can sit away from its entity's origin: `offsetPosition` (in the
entity's local axes) and `offsetRotation`. `colliderLocalTransform(collider,
scale)` is the one convention for where the shape sits in its body (the
entity's world position and rotation, unscaled): the offset position is
multiplied by the entity's world scale, then the offset rotation turns the
shape, whose dimensions are scaled on its own axes as without an offset.
`PhysicsWorld` wraps such a shape in a Jolt `RotatedTranslatedShape` (outside
the mesh shape cache, so a cached mesh shape stays shared),
`gatherNavigationGeometry` places its triangles the same way, and so does the
editor's collider gizmo; `tests/editor/SceneGizmoDrawTests.cpp` checks that
the three agree. The `collider` component is schema version 2: it writes
`"offset": {"position": [x, y, z], "rotation": [x, y, z, w]}` only when the
shape is moved or turned; version 1 documents have none and load unchanged.

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
- `GameServices::publishPhysicsEvents` turns them into `TriggerEntered` and
  `TriggerExited` events.

Event order is deterministic, so a seeded run replayed with the same input
sees the same sequence. Jolt reports contacts from its worker threads in any
order; the step sorts them before turning them into events:
- `contactEvents()` follows `contactEventPrecedes`: the pair's lower, then
  higher scene id, then type (Begin, Persist, End), then the first body's
  scene id and the normal. Each contact also carries both scene ids.
- `triggerEvents()` lists the exits of removed bodies first, then the step's
  own events, each group by sensor scene id, other scene id, then type
  (`triggerEventPrecedes`). A pair's new contacts count before its ended ones,
  so a body swapping touching shapes inside a sensor does not flicker out and
  back in.
- Characters, and bodies the audit or `clear` removes, are removed in scene id
  order, so Jolt reuses freed body ids the same way every run.
- `GameServices` hands the events to the `EventQueue` and the behaviours'
  trigger and collision hooks in that order.

`step` fails only before `init`; content problems degrade instead of ending
the run, so an outlier generated level keeps playing:

- An entity whose body cannot be built (a triangle-mesh collider on a moving
  body, a transform that cannot be decomposed, such as a zero scale) or whose
  character capsule is not positive is left out of the simulation with one
  warning naming it. It is retried when it changes (a lifecycle event) and on
  audits. `refreshBody` still reports the failure as an error.
- Bodies beyond `maxBodies` are skipped with one warning per episode and
  built, lowest entity first, once removals make room.
- A step that runs out of body pairs or contact constraints completes without
  the contacts that did not fit (bodies may pass through each other) and warns
  once per episode. Debug builds of Jolt assert on that error; the runtime's
  assert handler lets only that assertion continue.

Triangle-mesh shapes are cached per world by (mesh index, generation, scale
in steps of 1e-5), so static colliders that share a mesh and scale, such as
hundreds of identical generated walls, share one Jolt bounding-volume tree;
rotated copies share too, since rotation is not part of the shape.
Building the tree is the expensive part of a mesh body; a cache hit costs a
map lookup. A generation in the key means a collected or recycled mesh never
matches; the entries of meshes that are no longer live are dropped at the
first step (or body build) after the asset revision changes, and shutdown
releases the rest. Bodies hold their own shape references, so dropping an
entry never affects one. To change a generated mesh, create its replacement,
point the colliders at it and mark them with `markPhysicsDirty`.
`cachedMeshShapes()` reports the cache's size.

`PhysicsStats::skippedBodies` and `contactCapacityExceeded` report both. The
capacities are `PhysicsWorldConfig` fields (`Engine::init` takes the config).
Their defaults and approximate memory per initialized world (the editor's edit
and Play worlds each pay it):

| Field | Default | Cost |
| --- | --- | --- |
| `maxBodies` | 8192 (was 4096) | about 25 bytes per body of tables, plus broad-phase nodes allocated as bodies are added |
| `maxBodyPairs` | 32768 (was 16384) | 80 bytes per pair across Jolt's two contact caches: 2.6 MB |
| `maxContactConstraints` | 16384 (was 10240) | 328 bytes per constraint across the two caches (5.4 MB), plus `contactConstraintTemporaryBytes` (512) of temporary memory per step |
| `temporaryAllocatorBytes` | 16 MB (was 10 MB) | reserved up front; `init` rejects a size below `maxContactConstraints` x 512 bytes, because Jolt aborts when a step outgrows it |

Together the new defaults cost about 9.5 MB more per world than the old ones
(3.3 MB of caches, 6 MB of temporary memory).

## `src/engine/physics/CharacterController.hpp`

`CharacterControllerComponent` (`character_controller`, schema version 2) is a
walking character that `PhysicsWorld` moves with Jolt's `CharacterVirtual`
instead of a rigid body. An entity with it gets no rigid body, even if it has
a collider and rigid body too. Each step, before the bodies are simulated, the
world creates, rebuilds (after a capsule, layer, or slope change) or removes
each character, then:

- blends the horizontal velocity toward `desiredVelocity` at `acceleration`
  (scaled by `airControl` in the air), cancelling movement into slopes
  steeper than `slopeLimitRadians`;
- on the ground, adds the ground's velocity (so a character rides a moving
  platform) and a requested jump, then gravity;
- runs `ExtendedUpdate`: walks onto steps up to `stepHeight`, and sticks to
  ground within `groundProbeDistance` going down;
- writes the new position into the entity's transform (keeping its rotation
  and scale), and fills `grounded`, `groundNormal`, `groundVelocity`,
  `velocity`, and `groundDistance` (a sphere cast below the capsule).

`jumpRequested` is consumed by every step. A transform moved by gameplay is a
teleport: the character follows it. Interpolation blends characters' positions
like dynamic bodies'. Each character has a kinematic inner body on its layer
whose user data is its entity, so queries, contacts, and sensors see the
character as its entity, and removing it exits the sensors it was in.

Version 1 was the demo's own controller; its migration drops the look
settings (now `FirstPersonCameraComponent`, see [Gameplay](gameplay.md)) and
adds the new fields at their defaults.

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
`DebugDraw`. The `layer` fields of `rigid_body` and `character_controller`
carry the `physicsLayerNameList` presentation hint
(`Fields::nameList`), so the editor's inspector chooses a layer by the
project's names.

## `src/engine/physics/JoltRuntime.*`

Private reference-counted owner of Jolt's process-wide state (factory,
registered types, the one debug renderer, and in Debug builds the assert
handler that lets a step's capacity error continue). Each initialized
`PhysicsWorld` holds a move-only `JoltRuntimeLease`; worlds coexist and shut
down in any order.

## `src/engine/physics/JoltDebugDraw.*`

Private adapter from Jolt's `DebugRendererSimple` to `DebugDraw`: lines and
triangle edges become debug lines, 3D text becomes labels. It needs Jolt's
`JPH_DEBUG_RENDERER`, which `cmake/Dependencies.cmake` enables in every
configuration.
