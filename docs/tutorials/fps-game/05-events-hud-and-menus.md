# 5. Events, the HUD, and menus

[Back to the outline](README.md)

## Events between behaviours

The weapon, the targets, and the score do not know each other's types.
They talk through `GameServices::events()`. An event is a copyable struct
with a unique `eventKey`, declared in `src/FpsGame.hpp`:

```cpp
struct TargetDestroyed {
    static constexpr std::string_view eventKey = "range_day.target_destroyed";
    SceneEntityId target;
};
```

`Target::takeDamage` publishes it and destroys the target through
`services.world().commands()`, which applies after the phase. The `score` system
reads the previous frame's events:

```cpp
for (const TargetDestroyed& destroyed : context.services->events().read<TargetDestroyed>()) {
    ++state.targetsDestroyed;
}
```

The exit's **Goal Zone** shows the trigger hooks: the physics world reports a
character entering the sensor, and `onTriggerEnter` publishes `GoalReached`.

## The HUD

The `hud` system is registered with `SystemPhase::Ui`, the phase that runs
inside the game's UI frame, and draws with `context.ui` (`GameUiContext`),
which is set only in that phase:

```cpp
ui.panel({{-1.0f, -10.0f}, {2.0f, 20.0f}}, {.color = {1, 1, 1, 0.85f}}, UiAnchor::Center);
ui.text({24.0f, 20.0f}, "Targets " + std::to_string(state.targetsDestroyed) + " / " +
                            std::to_string(state.targetsTotal), 28.0f);
```

Positions are offsets from the anchor. The same system draws in the editor's
game view and in `zenith --project`.

> 📷 **Screenshot needed — `TUT-FPS-05-HUD`:** *The game view showing the HUD (crosshair and "Targets n / m") and, separately, the pause menu.*

## Effects

The weapon spawns `prefabs/impact.prefab` where a shot lands:

```cpp
context.services->world().effects().spawn(impactEffect, glm::translate(glm::mat4(1.0f), hit.position), {.pooled = true});
```

The prefab is one entity with a one-shot particle burst, so the effect
removes itself when the burst ends. Make your own with **Create Prefab from
Selection** on an entity with a Particle Emitter.

## Pause and settings

`drawPauseMenu` and `drawSettingsMenu` are the engine's menus; the game
supplies their entries. The template's pause calls
`context.services->time().setPaused(true)`: physics, FixedUpdate, timers,
animation, scene audio, and the player's look stop, while Update systems
keep running with a zero `deltaSeconds` (and the real frame time in
`unscaledDeltaSeconds`), so the score system can read the Pause key and
resume. The settings change `FirstPersonViewPreferences`, which the camera
system reads.

Next: [Debug, test, and ship](06-debug-test-and-ship.md).
