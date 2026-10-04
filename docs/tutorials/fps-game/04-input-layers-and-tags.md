# 4. Input, layers, and tags

[Back to the outline](README.md)

**Edit > Project Settings** edits what the whole project shares. Apply saves
it to the project files and applies it at once, in Edit mode.

## Input actions

The game never reads keys. It reads **input actions**: named buttons and
axes (`Fire`, `Move`) bound to keys, mouse buttons and axes, and gamepad
controls. The project's actions are in `input_actions.json`; a module can
also declare defaults in code with `registrar.addInputActions`, and the
project's file replaces those of the same name.

In the **Input Actions** tab, select `Fire` and press **Add Binding**. Choose
`key:F` as the source. Play: F fires too. The file now lists the binding:

```json
{"name": "Fire", "type": "button", "bindings": [
  {"source": "mouseButton:Left"}, {"source": "gamepadButton:RightShoulder"},
  {"source": "key:F"}]}
```

A behaviour reads an action through `BehaviourContext::actions`:
`context.actions->pressed("Fire")`, `axis2D("Move")`.

## Physics layers

The **Physics Layers** tab names collision layers and which pairs collide.
Add a layer `Targets` and uncheck its pair with `Player`, so the player walks
through targets but shots still hit them. Then set each target's rigid body
**Layer**, which the inspector now offers by name.

A ray can also filter by layer. `PhysicsQueryFilter::layerMask`
(`physicsLayerMask({...})`) limits what it hits, and `ignore` skips entities;
the Hitscan Weapon ignores the player that carries it.

## Tags

The **Tags** tab lists the project's tags; the template declares `target`.
Add a `Tags` component to an entity and pick tags from the list. A name the
project does not declare is marked, since game code may have misspelled it.
Game code finds tagged entities with `findEntitiesWithTag(scene, "target")`
and checks one with `hasTag`.

Next: [Events, the HUD, and menus](05-events-hud-and-menus.md).
