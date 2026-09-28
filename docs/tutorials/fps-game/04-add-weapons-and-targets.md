# 4. Add a hitscan weapon and targets

Implement weapons as game-side data and systems. Physics supplies a reusable
raycast mechanism; it should not know about ammunition, damage, teams, or
scores.

## Components

```cpp
struct HealthComponent {
    float current{100.0f};
    float maximum{100.0f};
};

struct HitscanWeaponComponent {
    float damage{25.0f};
    float range{100.0f};
    float shotsPerSecond{6.0f};
    int magazineSize{12};
    int ammunition{12};
    float cooldownSeconds{0.0f};
    Entity camera{entt::null};
};
```

Attach the weapon to the player and point it at the player's camera. Attach
`HealthComponent` to target entities. Use primitive static or dynamic colliders
for every shootable target; raycasts only hit physics bodies.

## Fire from the camera

Update mouse look first, finalize world transforms, and then process the shot:

```cpp
m_controllers.updateView(context.scene, input);
context.scene.updateWorldTransforms();
m_weapons.update(context.scene, context.physics, input, deltaSeconds);
```

In `HitscanWeaponSystem::update`:

```cpp
weapon.cooldownSeconds = std::max(0.0f, weapon.cooldownSeconds - deltaSeconds);
if (!input.mouseButtonPressed(MouseButton::Left) ||
    weapon.cooldownSeconds > 0.0f || weapon.ammunition == 0) {
    continue;
}

const auto& cameraTransform = registry.get<TransformComponent>(weapon.camera);
const glm::vec3 origin = glm::vec3(cameraTransform.world[3]);
const glm::vec3 direction = -glm::normalize(glm::vec3(cameraTransform.world[2]));
PhysicsRayHit hit;
if (physics.raycast(origin, direction, weapon.range, hit)) {
    if (auto* health = registry.try_get<HealthComponent>(hit.entity)) {
        health->current = std::max(0.0f, health->current - weapon.damage);
    }
}
--weapon.ammunition;
weapon.cooldownSeconds = 1.0f / weapon.shotsPerSecond;
```

Validate the camera entity before reading its transform, and reject zero or
negative fire rates during setup. `PhysicsRayHit` also contains the hit point,
surface normal, and normalized fraction along the ray; use those later for
impact effects and decals.

## Avoid frame-rate-dependent weapons

Cooldowns can be updated in the variable step for a simple semi-automatic
weapon. For deterministic automatic fire, accumulate elapsed time and emit a
bounded number of shots per update. Do not multiply damage by frame time.

Reload can initially use `Key::R`, but the demo already uses R for scene
reload. Choose one meaning per game state or extend the engine's physical-key
enum and SDL mapping deliberately. Input action mapping is a useful future
game-layer abstraction; it should not hard-code FPS actions into
`InputManager`.

## Destroy targets safely

Do not destroy entities while iterating the same EnTT view. Collect defeated
entities, then remove them after the iteration:

```cpp
for (Entity entity : defeated) {
    scene.destroyEntity(entity);
}
```

`Scene::destroyEntity` recursively removes descendants. Physics notices absent
entities on the next fixed step and releases their bodies.

## Useful next weapon features

- Add spread by perturbing the direction inside a game-owned random stream.
- Apply `PhysicsWorld::applyImpulse` to dynamic hit entities.
- Store teams in a component and reject friendly hits.
- Pool short-lived impact entities instead of allocating without bounds.
- Keep visual recoil on a weapon/camera child, not on the rigid-body transform.
