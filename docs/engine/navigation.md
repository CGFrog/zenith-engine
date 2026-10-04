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
  ranges. Dynamic and kinematic bodies and sensors are left out.
- `bake` builds one tile covering the scene from `NavMeshBuildSettings`
  (cell size, the agent's height, radius, climb, and steepest slope, region
  and edge tuning) and replaces any previous mesh. It fails, leaving no mesh,
  when nothing is walkable.
- `findPath` returns the corners of the shortest path, start and end included;
  it fails when either point is off the mesh or the end is unreachable (Detour
  would otherwise return a partial path). `nearestPoint` snaps a point onto
  the mesh.
- `drawDebug` records the polygon edges into a `DebugDraw`.

## `NavAgent.*`

`NavAgentComponent` (`nav_agent`: speed and stopping distance) walks an entity
to a destination set with `setNavDestination`. `updateNavAgents` plans a path
when the destination changes, skips the corners it has reached, and steers
toward the next one: through the entity's `CharacterControllerComponent`
(`desiredVelocity`) when it has one, otherwise by moving its transform along
the path. A character an agent drives should have an empty move action, so
player input does not drive it. On arrival the destination clears and
`arrived` is set; an unreachable destination sets `pathFailed`.

The engine's `zenith.nav_agents` system (Update, Play) runs it on the
session's mesh, `GameServices::navigation(scene)`, which bakes on first use.
In the editor, `navigation.bake` builds a preview mesh of the edit scene that
every viewport draws, and `navigation.clear` removes it.
