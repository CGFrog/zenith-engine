# Navigation (`src/engine/navigation/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `NavigationWorld.*`

A navigation mesh over a scene's static geometry. Recast builds it and Detour
queries it; both stay private (`zenith_recast` is linked privately into the
engine).

- `gatherNavigationGeometry` turns the static, solid colliders on a layer mask
  into triangles, shaped as `PhysicsWorld` builds their bodies: boxes, and
  spheres and capsules as coarse meshes, all wound outward so Recast finds the
  upward faces; triangle-mesh colliders use their `MeshAsset`'s full-detail
  ranges. Dynamic and kinematic bodies and sensors are left out. Collider
  poses come from `Scene::worldTransform`, so a bake right after spawning or
  moving entities sees them in place without a transform update first.
- `appendMeshGeometry` adds a mesh's full-detail triangles placed by a
  transform (reversing the winding for a mirroring one), so a game can bake
  generated geometry, such as a runtime floor mesh without a collider, with
  or without the gathered colliders.
- `bake` builds one tile covering the scene from `NavMeshBuildSettings`
  (cell size, the agent's height, radius, climb, and steepest slope, region
  and edge tuning) and replaces any previous mesh. It fails, leaving no mesh,
  when nothing is walkable.
- `findPath` returns the corners of the shortest path, start included, and
  says how far it gets (`NavPath`). `Complete` ends at the end point.
  `Partial` means the end cannot be reached (another island, or no way round
  a wall), and the path ends at the reachable point nearest it. `None`
  (empty) means a point is off the mesh or nothing is baked. `nearestPoint`
  snaps a point onto the mesh.
- `generation()` changes whenever the mesh is baked or cleared, so the crowd
  knows to start over.
- `drawDebug` records the polygon edges into a `DebugDraw`.

## `NavigationCrowd.*`

Agents moving over one `NavigationWorld`'s mesh together, steering around
each other: Detour's `dtCrowd` (DetourCrowd, built into `zenith_recast`),
which stays private.

- `reset(world, NavCrowdSettings)` starts over on the world's mesh and
  removes every agent. The settings are the maximum agent count, the largest
  agent radius, and the separation weight.
- `addAgent(position, {radius, height, maximumSpeed}, owner)` returns an
  index, or -1 when the crowd is full or the position is off the mesh. Each
  slot records the owner it was added for (an entity id), so `owns` tells a
  caller's slot from one reused for someone else.
- `requestMove`, `stop` and `placeAgent` (where the agent really is, for
  example after physics moved it) come before `update`. After it, `state`
  gives the agent's position, velocity, where its path ends, whether that
  path is partial, and whether it is moving.
- Agents accelerate almost at once (100 m/s²). A character controller
  smooths its own motion, so the crowd adds no second ramp.
- `mark` and `sweepUnmarked` remove agents whose owners stopped claiming
  them.

## `NavAgent.*`

`NavAgentComponent` (`nav_agent`: speed, stopping distance, and radius)
walks an entity to a destination set with `setNavDestination`.
`updateNavAgents(scene, world, crowd, seconds, settings)` drives every agent
through the crowd:
- It resets the crowd when the world's generation changed, so agents rejoin
  after a rebake.
- It adds each agent (idle ones too, so others walk around them), places it
  where its entity stands, and requests its destination when that changed.
- After the crowd steps, an agent follows its crowd velocity: through the
  entity's `CharacterControllerComponent` (`desiredVelocity`) when it has
  one, otherwise by moving its transform to its crowd position.
- Agents of destroyed entities leave the crowd, and a copied component (a
  duplicated entity) takes its own slot.

A character an agent drives should have an empty move action, so player
input does not drive it.

Arrival and failure:
- Within the stopping distance of its path's end, the destination clears and
  `arrived` is set.
- An unreachable destination makes a partial path: the agent arrives as near
  as the mesh allows, with `partialPath` set.
- A destination off the mesh, or a full crowd, sets `pathFailed`.

The engine's `zenith.nav_agents` system (Update, Play) runs it on the
session's mesh and crowd (`GameServices::navigationCrowd`, with
`setCrowdSettings` applied when the crowd next starts over).
`GameServices::bakeNavigation(scene)` rebakes the mesh after a game builds or
replaces its level, and `bakeNavigation(geometry)` bakes it from triangles the
game assembled itself (`gatherNavigationGeometry` plus `appendMeshGeometry`).
When the first bake happens is the session's
`NavigationBakePolicy`:
- `Lazy` (the default, which hand-authored levels rely on):
  `GameServices::navigation(scene)` bakes the first time it is asked, and the
  agent system asks as soon as an agent exists.
- `Explicit`: only `bakeNavigation` bakes. Until it has run, the agent system
  skips the agents, which keep their destinations (no `pathFailed`), and logs
  one warning; `navigation(scene)` returns the empty mesh.

A game that generates its levels sets `Explicit`
(`services.setNavigationBakePolicy`) before the first Update that sees an
agent, for example in the system that builds the floor. Under `Lazy`, enemies
spawned before the game's own bake would make the agent system bake the
half-built floor first, and the game's bake would be a second one. A 40-room floor (2,880 input triangles) bakes in about
26 ms in Release (`tests/engine/FloorBenchmark.cpp`).
In the editor, `navigation.bake` builds a preview mesh of the edit scene that
every viewport draws, and `navigation.clear` removes it.
