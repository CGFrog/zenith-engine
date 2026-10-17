# 6. Gameplay services

[Guide contents](README.md) · Previous: [Writing game logic](05-game-logic.md) · Next: [How the engine works](07-engine-concepts.md)

Systems and behaviours reach the engine through a few objects in their
context. This chapter is a tour of each, with the code you will actually
write. Most of them hang off **`GameServices`** (`context.services`), which
exists from the moment Play starts until it stops (in the game runner: from
scene load to unload).
`GameServices` groups its services by the question they answer:
`world()` changes the scene (commands, prefabs, effects), `time()` says when
things happen (timers, pause, time scale), `content()` is the session's own
data (resources, random streams, asset paths), and `outside()` reaches
beyond the scene (audio, navigation, saves, debug drawing). Events and
logging (`events()`, `log()`) sit on `GameServices` itself.

| You want to... | Use | Section |
| --- | --- | --- |
| Read the player's buttons and sticks | `context.actions` | [Input](#input-actions) |
| Raycast, sweep, overlap | `context.physics` | [Physics queries](#physics-queries) |
| Spawn, destroy, reparent | `services->world().commands()` | [Spawning and destroying](#spawning-and-destroying) |
| One-shot visual effects | `services->world().effects()` | [Effects](#effects) |
| Tell other code something happened | `services->events()` | [Events](#events) |
| Do something later or repeatedly | `services->time().timers()` | [Timers](#timers) |
| Draw a HUD or a menu | `context.ui` (Ui phase) | [UI](#hud-and-menus) |
| Play sounds | `services->outside().audio()` | [Audio](#audio) |
| Pause or slow time | `services->time().setPaused`, `setTimeScale` | [Time](#time-pause-and-slow-motion) |
| Reproducible randomness | `services->content().random(name)` | [Randomness](#randomness) |
| Data files (items, enemies, loot) | `DefinitionTable<T>` | [Data tables](#data-tables) |
| Keep state for a run | registry context / `services->content().resource<T>()` | [Per-game state](#per-game-state) |
| Move AI along paths | `NavAgentComponent`, `services->outside().navigation()` | [Navigation](#navigation) |
| Drive animation | `AnimatorComponent` | [Animation](#animation) |
| Save the game | `services->outside().saveGame(...)` | [Save games](#save-games) |
| Log and debug-draw | `services->log(name)`, `services->outside().debugDraw()` | [Chapter 4](04-play-and-debug.md#debugging-tools) |

## Project settings

**Edit > Project Settings...** edits what the whole project shares. **Apply**
saves it to the project's files and takes effect immediately, even in Edit
mode.

| Tab | Holds |
| --- | --- |
| **Input Actions** | Named actions and their bindings (`input_actions.json`). |
| **Physics Layers** | Up to 32 named collision layers and which pairs collide. |
| **Tags** | The project's tag names. |
| **Navigation** | Navigation-mesh bake settings (agent size, slope, step). |

> 📷 **Screenshot needed — `GUIDE-06-PROJECTSETTINGS`:** *Project Settings on the
> Physics Layers tab, showing the layer list (Static, Dynamic, Player, plus a
> custom "Targets" layer) and the collision matrix of checkboxes.*

## Input actions

Game code never reads keys directly. It reads **actions**: named buttons and
axes bound to keys, mouse buttons and axes, and gamepad controls. Players can
rebind them, and keyboard and gamepad work the same way in your code.

```json
{
  "version": 1,
  "actions": [
    {"name": "Move", "type": "axis2d", "clamp": true, "bindings": [
      {"source": "key:W", "scale": [0, 1]},  {"source": "key:S", "scale": [0, -1]},
      {"source": "key:D", "scale": [1, 0]},  {"source": "key:A", "scale": [-1, 0]},
      {"source": "gamepadAxis:LeftX", "scale": [1, 0]},
      {"source": "gamepadAxis:LeftY", "scale": [0, -1]}
    ]},
    {"name": "Fire", "type": "button", "bindings": [
      {"source": "mouseButton:Left"}, {"source": "gamepadButton:RightShoulder"}
    ]}
  ]
}
```

Sources are `key:<Name>`, `mouseButton:<Left|Right|Middle>`,
`mouseAxis:<DeltaX|DeltaY>`, `gamepadButton:<South|East|Start|...>`, and
`gamepadAxis:<LeftX|LeftY|RightX|RightY|...>`. A binding may set `scale`,
`deadZone`, and `modifiers`.

Reading them:

```cpp
const InputActionState* actions = context.actions;   // null when no player input
if (actions == nullptr) { return; }

if (actions->pressed("Fire")) { ... }    // went down this frame
if (actions->down("Sprint"))  { ... }    // held
if (actions->released("Aim")) { ... }    // went up this frame
const glm::vec2 move = actions->axis2D("Move");   // x right, y forward
const float throttle = actions->axis("Throttle");
```

A module can declare default actions in code with
`registrar.addInputActions(map)`; the project's `input_actions.json`
replaces actions of the same name. Deep dive:
[Window and input](../engine/window-input.md).

> 📷 **Screenshot needed — `GUIDE-06-INPUTACTIONS`:** *Project Settings on the
> Input Actions tab with "Fire" selected, its bindings listed, and the Add
> Binding source picker open.*

## Physics queries

`context.physics` (behaviours) is the scene's `PhysicsWorld`
(`engine/physics/PhysicsWorld.hpp`). Queries see the bodies as of the last
physics step.

```cpp
#include "engine/physics/PhysicsWorld.hpp"

// The camera looks down its local -Z axis.
const glm::mat4 world = scene.worldTransform(context.entity);
const glm::vec3 origin(world[3]);
const glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));

const std::array<Entity, 1> ignored{context.entity};
PhysicsRayHit hit;
if (context.physics->raycast(origin, forward, 100.0f, hit, {.ignore = ignored})) {
    // hit.entity, hit.position, hit.normal, hit.fraction (0..1 along the ray)
}
```

| Query | Finds |
| --- | --- |
| `raycast(origin, dir, distance, hit, filter)` | The nearest hit. |
| `raycastAll(origin, dir, distance, hits, filter)` | Every body along the ray, nearest first. |
| `shapeCast(shape, start, dir, distance, hit, filter)` | The first body a swept sphere/box/capsule meets. |
| `overlap(shape, pose, entities, filter)` | Every body overlapping a shape. |
| `closestPoint(point, maxDistance, closest, filter)` | The nearest body surface. |

`PhysicsQueryFilter` limits what a query sees: `layerMask` (use
`physicsLayerMask({layerA, layerB})`), `ignore` (entities to skip, such as the
shooter), and `hitSensors`.

**Moving bodies from code.** Kinematic bodies follow their transform: move
them with `scene.setPosition` etc. Dynamic bodies are moved by physics: use
the world's force, impulse, and velocity functions (`setLinearVelocity`,
`linearVelocity`, ...) from `FixedUpdate`. A **Character Controller** is
driven by its `desiredVelocity` and `jumpRequested` fields; the engine's
`zenith.character_input` system fills them from the `Move`/`Jump`/`Sprint`
actions named on the component, or leaves them to your code when its move
action is empty (for AI).

**Contacts and triggers.** Behaviours get `onCollision`, `onTriggerEnter`,
and `onTriggerExit` (see [chapter 5](05-game-logic.md#hooks-and-the-order-they-run-in)).
Systems can read `TriggerEntered`/`TriggerExited` events, or the world's
`contactEvents()` and `triggerEvents()`. Deep dive: [Physics](../engine/physics.md).

## Spawning and destroying

Never create or destroy entities while iterating; record **commands** and the
engine applies them after the current phase:

```cpp
EntityCommandBuffer& commands = context.services->world().commands();

// Spawn a prefab (project-relative path) at a place, then configure it.
commands.spawn("prefabs/enemy.prefab",
               SpawnPlacement{.localTransform = glm::translate(glm::mat4(1.0f), spawnPoint)},
               [](Scene& scene, Entity spawned) {
                   scene.registry().get<HealthComponent>(spawned).current = 50.0f;
               });

commands.spawnEmpty("Marker", SpawnPlacement{.parent = scene.id(context.entity)});
commands.destroy(scene.id(enemy));                   // also destroys its children; runs onDestroy
commands.reparent(scene.id(item), scene.id(hand));
```

Spawns and destroys publish `EntitySpawned` and `EntityDestroyed` events. The
`onSpawned` callback must not keep the `Entity` after it returns; keep
`scene.id(entity)` instead.

## Effects

`effects().spawn(prefab, transform, options)` places a prefab that removes
itself when it is done: after `options.lifetime` seconds, or, if omitted,
after the longest particle lifetime of its one-shot emitters. Set
`.pooled = true` for frequent effects (impacts, muzzle flashes): finished
instances are parked and reused instead of destroyed.

```cpp
context.services->world().effects().spawn("prefabs/impact.prefab",
                                  glm::translate(glm::mat4(1.0f), hit.position), {.pooled = true});
```

## Events

Events let parts of your game talk without knowing each other's types. An
event is a copyable struct with a unique `eventKey`:

```cpp
struct TargetDestroyed {
    static constexpr std::string_view eventKey = "my_game.target_destroyed";
    SceneEntityId target;
};

// Publisher (e.g. a behaviour):
context.services->events().publish(TargetDestroyed{.target = scene.id(context.entity)});

// Reader (e.g. a score system):
for (const TargetDestroyed& event : context.services->events().read<TargetDestroyed>()) {
    ++score;
}
```

**Timing:** an event published in frame *N* is readable in every phase of
frame *N + 1*, then dropped. This one-frame delay makes the order of systems
irrelevant to who sees what. Keys are strings (not C++ type ids) so the engine
and every module agree on them; prefix yours with your game's name.

Engine events (`engine/module/GameEvents.hpp`): `SceneLoaded`,
`EntitySpawned`, `EntityDestroyed`, `TriggerEntered`, `TriggerExited`.

## Timers

```cpp
Timers& timers = context.services->time().timers();

// Once, after 3 seconds of game time; dropped automatically if `owner` is destroyed.
TimerHandle handle = timers.start(3.0f, TimerRepeat::Once, [](TimerContext& timer) {
    // timer.scene, timer.owner
}, scene.id(context.entity));

timers.start(0.5f, TimerRepeat::Repeat, spawnWave);   // every half second
timers.cancel(handle);
```

Timers run on the game clock, so they follow the time scale and stop while
paused. A callback that throws is logged and its timer dropped.

## HUD and menus

Register a system in the `Ui` phase; there, `context.ui` is a
`GameUiContext` (`engine/ui/GameUi.hpp`). UI is immediate mode: you describe
the UI every frame and widgets return what happened.

```cpp
void drawHud(const SystemContext& context) {
    GameUiContext& ui = *context.ui;
    // A crosshair: a small panel at the screen centre.
    ui.panel({{-1.0f, -10.0f}, {2.0f, 20.0f}}, {.color = {1, 1, 1, 0.85f}}, UiAnchor::Center);
    // Text 24 units from the top-left corner, 28 units tall.
    ui.text({24.0f, 20.0f}, "Score " + std::to_string(score), 28.0f);
    ui.progressBar({{24.0f, 60.0f}, {300.0f, 16.0f}}, health / maxHealth);
    if (ui.button("restart", {{-120.0f, -40.0f}, {240.0f, 56.0f}}, "Restart", UiAnchor::BottomCenter)) {
        restartRequested = true;
    }
}

registrar.addSystem({.name = "my_game.hud", .phase = SystemPhase::Ui, .run = drawHud});
```

- Coordinates are in a **1920×1080 design canvas**, scaled uniformly to the
  window. A position is an offset from its **anchor** (`TopLeft`,
  `TopCenter`, `TopRight`, `CenterLeft`, `Center`, `CenterRight`,
  `BottomLeft`, `BottomCenter`, `BottomRight`), so a HUD stays in its corner
  at any aspect ratio.
- Widgets: `panel`, `text`, `image`, `button`, `toggle`, `slider`,
  `progressBar`, plus `UiLayout::column`/`row` to stack rects and
  `pixelText` for labels that follow world positions.
- Keyboard and gamepad navigation are built in (`setNavigation`), along with
  accessibility text scaling.
- For mouse-driven menus, free the pointer while the menu is open:
  `services->outside().setPointerReleased(menuOpen)`. The game takes the
  mouse back when you clear it, in the editor's game view and in the shipped
  game alike.
- Ready-made screens (`engine/ui/GameMenus.hpp`): `drawPauseMenu` returns
  `Resume`, `OpenSettings`, or `Quit`; `drawSettingsMenu` draws toggles,
  choices, and sliders from `SettingsEntry` values you supply and returns
  `Changed` or `Back`.

The same UI draws in the editor's game view and in the shipped game.

> 📷 **Screenshot needed — `GUIDE-06-HUD`:** *The FPS template's HUD in the game
> view: crosshair, "Targets 2 / 5" counter, and a message line; and a second
> capture of its pause menu.*

Deep dive: [Runtime game UI](../engine/ui.md).

## Audio

**In the editor:** add an **Audio Emitter** with a clip asset (WAV or Ogg) for
ambient or positional sound, and an **Audio Listener** on the camera (the
first-person player gets one automatically).

**From code:**

```cpp
GameServices& services = *context.services;
if (AudioSystem* audio = services.outside().audio()) {             // null without an audio device, e.g. tests
    const auto clip = services.outside().loadClip("audio/shot.wav");
    if (clip) {
        AudioSpatialOptions options;
        options.position = hit.position;
        options.gain = 0.8f;
        audio->playAt(clip.handle, options);             // positional; play() is non-positional
    }
}
```

Voices go to the **Master**, **Music**, or **Sfx** bus. Spatial audio uses
inverse-distance attenuation and equal-power stereo panning; set
`options.follow` to an entity id to make the sound move with it. Deep dive:
[Audio](../engine/audio.md).

## Time: pause and slow motion

```cpp
services.time().setPaused(true);       // physics, FixedUpdate, timers, animation, scene audio stop
services.time().setTimeScale(0.25f);   // slow motion
```

While paused, Update and LateUpdate systems still run with
`deltaSeconds == 0` (so they can read a "resume" button); use
`unscaledDeltaSeconds` for anything that must keep moving, like menu
animations. The editor's own pause and time scale multiply with the game's.

## Randomness

`services.content().random("loot")` is a named, seeded random stream (PCG32) whose
results are identical on every platform, compiler, and build type:

```cpp
RandomStream& rng = services.content().random("loot");
const int roll = rng.range(1, 6);                 // both ends inclusive
const bool crit = rng.chance(0.15f);
const size_t pick = rng.weightedIndex(weights);
rng.shuffle(deck);
```

Each name gets an independent stream derived from the session seed and the
name, so drawing more from `loot` never changes `layout`. Reseed when a run
begins (`randomStreams()`), and store `randomStreams().snapshot()` in a save
game to resume exactly. Deep dive: [Gameplay › Random](../engine/gameplay.md#random).

## Data tables

Keep tunable content (items, enemies, waves) in JSON files instead of code:

```cpp
struct ItemDef {
    std::string id;
    std::string name;
    float weight{1.0f};
};

Fields<ItemDef> itemFields() {
    return Fields<ItemDef>().add("id", &ItemDef::id).add("name", &ItemDef::name).add("weight", &ItemDef::weight);
}

struct ItemTable {
    static constexpr std::string_view resourceKey = "my_game.items";
    DefinitionTable<ItemDef> table{itemFields()};
};

ItemTable& items = services.content().resource<ItemTable>();
items.table.load(services.content().assetPath("data/items.json"));
if (const ItemDef* sword = items.table.find("sword")) { ... }
```

Loading is strict: a typo'd key, wrong type, or duplicate id is an error that
names the file, entry, and field (`items.json[3] 'sword': unknown field
'wieght'`). `reloadIfChanged()` picks up edits while the game runs. Register
the schema with `registrar.addDefinitionTable<ItemDef>("items", itemFields())`
and the Asset Browser validates the file as you edit it.

## Per-game state

Where to keep state depends on how long it should live:

| Lifetime | Store it in | Example |
| --- | --- | --- |
| Per entity, saved with the scene | A component field | A door's open height. |
| Per entity, runtime only | A behaviour's private member, or a field marked `.runtimeState()` | A cooldown timer. |
| Per scene / per Play | The registry context: `scene.registry().ctx().emplace<T>()` | The round's score. A new Play starts fresh. |
| Per session, shared across modules | `services.content().resource<T>()` (needs `static constexpr std::string_view resourceKey`) | Loaded data tables, the current run. |
| Across sessions | Save games | Progress. |

**Never** keep game state in module-level `static` variables: hot reload
throws them away. The FPS template's `fpsGameState(scene)` shows the registry
context pattern:

```cpp
FpsGameState& fpsGameState(Scene& scene) {
    auto& context = scene.registry().ctx();
    if (FpsGameState* state = context.find<FpsGameState>()) {
        return *state;
    }
    return context.emplace<FpsGameState>();
}
```

## Navigation

1. Give the level static colliders, then **Object > Navigation > Bake** (or
   the viewport sidebar's **Navigation** tab) to preview the mesh. Settings
   are on **Project Settings > Navigation**.
2. Give AI entities a **Navigation Agent** (`nav_agent`): speed, stopping
   distance, radius.
3. From code, `setNavDestination(agent, position)`; the built-in
   `zenith.nav_agents` system steers it and sets `arrived`.

For custom movement, `services.outside().navigation(scene).findPath(start, end, path)`
fills a list of corner points. In Play the mesh is baked from the scene's
static colliders on first use. Deep dive: [Navigation](../engine/navigation.md).

> 📷 **Screenshot needed — `GUIDE-06-NAVMESH`:** *A level with the Navigation
> Mesh overlay on, the sidebar's Navigation tab showing bake stats, and an
> agent with its path drawn.*

## Animation

Models with glTF animation clips get an **Animation** component. For
gameplay-driven animation add an **Animator**: a state machine of states
(clip, speed, loop), transitions with conditions, and parameters. From code:

```cpp
auto& animator = scene.registry().get<AnimatorComponent>(entity);
animator.setFloat("speed", glm::length(velocity));
animator.setBool("grounded", grounded);
animator.setTrigger("attack");
```

Event markers in clips are published as `AnimationEvent`s. Deep dive:
[Animation](../engine/animation.md).

## Save games

When the host provides a save store, `services.outside().saveGame(scene, "slot1",
"Chapter 2")` saves the scene the session plays, and `services.outside().saveGames()`
lists and loads slots. Generated (transient) entities are skipped, so
regenerate procedural levels from a saved seed. Deep dive:
[Scene](../engine/scene.md).

[Guide contents](README.md) · Previous: [Writing game logic](05-game-logic.md) · Next: [How the engine works](07-engine-concepts.md)
