# 5. Writing game logic

[Guide contents](README.md) · Previous: [Playing and debugging](04-play-and-debug.md) · Next: [Gameplay services](06-gameplay-services.md)

Your game's rules live in C++ in your project's **game module**. The engine
supplies mechanisms (physics, rendering, input, audio, UI); the module decides
what they mean for *your* game. This chapter covers the three building blocks
you write: **components**, **systems**, and **behaviours**.

## The mental model

```text
 Scene (an ECS registry)
 ├─ Entity "Player"   [Transform] [CharacterController] [Health]   ← components: plain data
 ├─ Entity "Door"     [Transform] [Model] [Collider] [Door]        ← Door is a behaviour: data + hooks
 └─ Entity "Crate"    [Transform] [Model] [Collider] [RigidBody]

 Systems: functions run every frame over all entities with given components
   regen_health(Update): for each [Health]  → health += rate * dt
   hud(Ui):              read the player's [Health] → draw a bar
```

| Building block | What it is | Use it for |
| --- | --- | --- |
| **Component** | A plain C++ struct with reflected fields. Saved in scenes, edited in the Inspector. | Data: health, speed, team, a spawn point's settings. |
| **System** | A function the engine calls once per frame phase over the scene. | Logic over many entities, or between them: movement, scoring, AI scheduling, the HUD. |
| **Behaviour** | A component class derived from `Behaviour` with lifecycle hooks (`onStart`, `onUpdate`, `onTriggerEnter`, ...). | Logic that belongs to *one* entity and reacts to its own events: a door, a pickup, a weapon. |

Rule of thumb: start with a behaviour when the logic is about "this thing";
move hot per-entity work into a system over a plain component when you have
many of them.

## The game module

A project made from the **C++ Game** template has this in `src/`:

```text
src/
|- CMakeLists.txt          lists the module's sources (keep the "# zenith:sources" marker)
|- GameModule.cpp          the module's entry points
|- SpinnerGameplay.hpp     an example component + system
`- SpinnerGameplay.cpp
```

`GameModule.cpp` names the module and registers everything:

```cpp
#include "SpinnerGameplay.hpp"
// zenith:includes (the editor's code generators add their headers above)

#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"

ZENITH_GAME_MODULE("my_game", "0.1.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registerSpinnerGameplay(registrar);
    // zenith:register (the editor's code generators add their types above)
}
```

- `ZENITH_GAME_MODULE(name, version)` defines the module's identity and the
  compatibility checks the engine runs when loading it (API version and a
  toolchain/runtime build hash). A module built against a different engine
  is refused with a clear message instead of crashing.
- `zenith_module_register` receives a `ModuleRegistrar`. Everything your game
  adds goes through it:

| Registrar call | Adds |
| --- | --- |
| `addComponentType(ComponentType)` | A component type (data). |
| `addSystem(SystemDescriptor)` | A system. |
| `addBehaviour<T>(key, displayName, Fields<T>, BehaviourOptions)` | A behaviour type. |
| `addInputActions(InputActionMap)` | Default input actions (the project's `input_actions.json` overrides same-named ones). |
| `addDefinitionTable<Entry>(name, Fields<Entry>)` | A data-table schema the Asset Browser validates. |

The editor loads the built module (`modules/<name>.zmodule`) and the game
runner loads the same file, so the code you test in the editor is the code
you ship.

## Let the editor write the boilerplate

**Module > New Behaviour...**, **New Component...**, and **New System...**
ask for a name and fields, then:

1. write `src/<Name>.hpp` and `src/<Name>.cpp` from the template's snippets;
2. add the `.cpp` above `# zenith:sources` in `src/CMakeLists.txt`;
3. add the include and `register<Name>(registrar);` above the `zenith:`
   markers in `GameModule.cpp`;
4. open the new file in your IDE and start a build.

Keep the three marker lines; the generators need them.

> 📷 **Screenshot needed — `GUIDE-05-NEWBEHAVIOUR`:** *The Module > New
> Behaviour... dialog with Name "Slider" and two float fields (distance,
> speed) filled in.*

## Components: data the editor understands

A component is a struct plus a description of its fields. The description
(`Fields<T>`) is what lets the scene file save it, the Inspector edit it, undo
track it, and prefabs override it. There is no macro magic or code scanning:
fields are listed explicitly with member pointers.

```cpp
// Health.hpp
#pragma once
#include <string_view>

class ModuleRegistrar;

struct HealthComponent {
    float current{100.0f};
    float maximum{100.0f};
    float regenPerSecond{0.0f};
};

inline constexpr std::string_view HealthComponentKey = "health";  // the key scenes save it under

void registerHealth(ModuleRegistrar& registrar);
```

```cpp
// Health.cpp
#include "Health.hpp"

#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"

#include <string>

namespace {
ComponentType healthComponentType() {
    return ComponentTypeBuilder<HealthComponent>(std::string(HealthComponentKey), "Health")
        .category("Gameplay")                     // the Add Component menu section
        .fields(Fields<HealthComponent>()
                    .add("current", &HealthComponent::current)
                    .drag(1.0f, 0.0, 10000.0)     // drag speed, min, max
                    .add("maximum", &HealthComponent::maximum)
                    .drag(1.0f, 1.0, 10000.0)
                    .add("regen_per_second", &HealthComponent::regenPerSecond)
                    .drag(0.1f, 0.0, 1000.0)
                    .format("%.1f hp/s")
                    .tooltip("Health restored each second while alive"))
        .build();
}
} // namespace

void registerHealth(ModuleRegistrar& registrar) {
    registrar.addComponentType(healthComponentType());
}
```

### Field options

Each `.add(key, &T::member)` can be followed by presentation options that
apply to that field:

| Option | Effect in the Inspector |
| --- | --- |
| `.drag(speed, min, max)` | Drag-to-scrub number field with limits. |
| `.slider(min, max)` | Slider. |
| `.format("%.2f s")` | Display format (units!). |
| `.angle()` | Shows radians as degrees. |
| `.color()` | Colour picker for a `glm::vec3`/`glm::vec4`. |
| `.tooltip(text)` / `.label(text)` | Hover help / display name. |
| `.readOnly()` / `.hidden()` | Shown but not editable / not shown. |
| `.optional()` | May be missing from older scene files. |
| `.asset(kind)` | An asset path field that accepts drops from the Asset Browser. |
| `.options({...})` | An enum shown as a dropdown. |
| `.nameList(list)` | Pick from a project list (e.g. physics layers). |
| `.runtimeState()` | Runtime-only state: not saved, visible while playing. |

Nested data uses `.structure(key, &T::member, Fields<Nested>())` and lists use
`.list(key, &T::member, Fields<Element>())`. A field of type `SceneEntityId`
is an **entity reference**: the Inspector shows a picker (and an eyedropper),
and duplicates and prefab instances remap it to the copied entity.

Supported field value types are `bool`, `int32_t`, `uint32_t`, `uint64_t`,
`float`, `glm::vec3`, `glm::vec4`, `glm::uvec3`, `std::string`, enums (with
`.options`), a min/max float pair (`.range(key, &T::min, &T::max)`), nested
structs, vectors of structs, and `SceneEntityId`. `FieldKind` in
`engine/reflection/Reflection.hpp` is the authoritative list.

### Changing a component later

Scenes store each component with its schema version. When you rename or
reshape fields after scenes already use them, bump the version and add a
migration on the builder so old files keep loading:

```cpp
ComponentTypeBuilder<HealthComponent>("health", "Health")
    .schemaVersion(2)
    .migration(1, [](nlohmann::json& json) -> SceneDocumentResult {
        json["maximum"] = json.value("max", 100.0f);   // v1 called it "max"
        json.erase("max");
        return SceneDocumentResult{.success = true};
    })
    // .validate([](const HealthComponent& h) { return h.maximum > 0 ? "" : "maximum must be positive"; })
    ...
```

## Systems: logic over the whole scene

A system is a named function plus *when* it runs:

```cpp
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>

namespace {
void regenerateHealth(const SystemContext& context) {
    auto healthy = context.scene.registry().view<HealthComponent>();   // an EnTT view
    for (const Entity entity : healthy) {
        HealthComponent& health = healthy.get<HealthComponent>(entity);
        health.current = std::min(health.maximum, health.current + health.regenPerSecond * context.deltaSeconds);
    }
}
} // namespace

void registerHealth(ModuleRegistrar& registrar) {
    registrar.addComponentType(healthComponentType());
    registrar.addSystem({.name = "my_game.regenerate_health",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Play,
                         .order = 0,
                         .run = regenerateHealth});
}
```

### Phases: when a system runs

| Phase | Runs | `deltaSeconds` is | Use it for |
| --- | --- | --- | --- |
| `Update` | Once per frame, before physics | The frame time on the game clock (0 while paused) | Input handling, game rules, timers. |
| `FixedUpdate` | Zero or more times per frame, once per fixed physics step (60 Hz by default) | The fixed step | Anything that drives physics: forces, velocities, character input. |
| `LateUpdate` | Once per frame, after physics and interpolation | The frame time on the game clock | Cameras and things that follow moving bodies. |
| `Ui` | Inside the UI frame | Wall-clock frame time | HUDs and menus through `context.ui`. |

Within a phase, systems run in ascending `order`; equal orders run in
registration order. The engine's built-in systems (character input, the
first-person camera, the behaviour runners, navigation agents, the animator)
run before a game's systems of the same phase and order.

`runIn` is `RunIn::Play` (the default), `RunIn::Edit`, or
`RunIn::EditAndPlay`. Edit-mode systems run in the editor without playing,
which is useful for previews and generators (draw with `context.debugDraw`).

### What a system receives: `SystemContext`

| Member | Meaning | Null when |
| --- | --- | --- |
| `scene` | The scene being run. | never |
| `deltaSeconds` | See the phase table. | – |
| `unscaledDeltaSeconds` | Wall-clock frame time, unaffected by pause or time scale. | – |
| `mode` | `SimulationMode::Play` or `Simulate`. | – |
| `actions` | The module's input actions, evaluated for this frame. | no player input (Edit, Simulate, unfocused game view) |
| `input` | Raw device state (prefer `actions`). | same as `actions` |
| `services` | `GameServices`: commands, events, timers, effects, audio, logs... ([chapter 6](06-gameplay-services.md)). | Edit mode |
| `ui` | `GameUiContext` for drawing UI. | every phase except `Ui` |
| `debugDraw` | Debug lines, shapes, labels. | hosts without one |
| `logger` | The engine logger. | – |

Always check the pointers you use: a system that runs in Edit mode gets no
services, and a system in Simulate gets no input.

## Behaviours: logic on one entity

```cpp
// Door.hpp
#pragma once
#include "engine/behaviour/Behaviour.hpp"
#include <string>

class ModuleRegistrar;

class Door final : public Behaviour {
  public:
    float openHeight{3.0f};
    float speed{2.0f};
    std::string openAction{"Interact"};

    void onStart(BehaviourContext& context) override;
    void onUpdate(BehaviourContext& context) override;
    void onTriggerEnter(BehaviourContext& context, Entity other) override;
    void onTriggerExit(BehaviourContext& context, Entity other) override;

  private:
    glm::vec3 m_closedPosition{0.0f};
    bool m_playerNear{false};
    bool m_open{false};
};

void registerDoor(ModuleRegistrar& registrar);
```

```cpp
// Door.cpp
#include "Door.hpp"

#include "engine/input/InputActions.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/physics/CharacterController.hpp"

#include <algorithm>

void Door::onStart(BehaviourContext& context) {
    m_closedPosition = context.scene.localPosition(context.entity);
}

void Door::onUpdate(BehaviourContext& context) {
    if (m_playerNear && context.actions != nullptr && context.actions->pressed(openAction)) {
        m_open = !m_open;
    }
    const glm::vec3 target = m_closedPosition + glm::vec3(0.0f, m_open ? openHeight : 0.0f, 0.0f);
    const glm::vec3 current = context.scene.localPosition(context.entity);
    const float step = speed * context.deltaSeconds;
    const glm::vec3 delta = target - current;
    const float distance = glm::length(delta);
    context.scene.setPosition(context.entity, distance <= step ? target : current + delta / distance * step);
}

void Door::onTriggerEnter(BehaviourContext& context, Entity other) {
    m_playerNear |= context.scene.registry().all_of<CharacterControllerComponent>(other);
}

void Door::onTriggerExit(BehaviourContext& context, Entity other) {
    if (context.scene.registry().all_of<CharacterControllerComponent>(other)) {
        m_playerNear = false;
    }
}

void registerDoor(ModuleRegistrar& registrar) {
    registrar.addBehaviour<Door>("door", "Door",
                                 std::move(Fields<Door>()
                                               .add("open_height", &Door::openHeight)
                                               .drag(0.05f, 0.0, 20.0)
                                               .format("%.2f m")
                                               .add("speed", &Door::speed)
                                               .drag(0.05f, 0.0, 20.0)
                                               .format("%.2f m/s")
                                               .add("open_action", &Door::openAction)));
}
```

Give the door entity a **Kinematic** rigid body so physics follows the moved
transform, and a child (or itself) with a **Sensor** collider so the trigger
hooks fire.

### Hooks and the order they run in

| Hook | When |
| --- | --- |
| `onCreate` | Once, the first time the engine sees the behaviour. |
| `onEnable` / `onDisable` | When `enabled` changes (`onEnable` right after `onCreate` if it starts enabled). |
| `onStart` | Once, before the first update of an enabled behaviour. Read your starting state here. |
| `onUpdate` | Every frame (Update phase). |
| `onFixedUpdate` | Every fixed step (FixedUpdate phase). |
| `onLateUpdate` | Every frame after physics (LateUpdate phase). |
| `onTriggerEnter` / `onTriggerExit` | A body entered or left this entity's sensor, or this entity entered another's sensor. |
| `onCollision(context, BehaviourCollision)` | A new contact with a solid body (`other`, `otherId`, `normal`). |
| `onDestroy` | The entity was destroyed through `services->world().commands()`, or the session stopped. |

Ordering is deterministic: by phase, then by the order behaviour types were
registered, then by entity creation order. Trigger and collision hooks run at
the start of the next FixedUpdate after the physics step that produced them.
Disabled behaviours get no update, trigger, or collision hooks.

Every behaviour automatically gets an `enabled` field in the Inspector. A
behaviour's section header also has **Edit Script** (opens the source in your
IDE) and a badge showing **Building**, **failed** (still running the previous
code), or **failed to load**.

> 📷 **Screenshot needed — `GUIDE-05-BEHAVIOURSECTION`:** *A behaviour's
> Inspector section (e.g. Hitscan Weapon) with the enabled checkbox, its
> fields with units, and the Edit Script button and status badge in the
> header.*

### What a hook receives: `BehaviourContext`

| Member | Meaning |
| --- | --- |
| `scene`, `entity` | The scene and this behaviour's entity. |
| `deltaSeconds` | Frame time on the game clock (the fixed step in `onFixedUpdate` and the physics hooks). |
| `unscaledDeltaSeconds` | Wall-clock frame time. |
| `services` | `GameServices` (null in Edit mode). |
| `actions` | Input actions (null without player input). |
| `physics` | The `PhysicsWorld` for raycasts and other queries (null in Edit mode and `onDestroy`). |
| `logger` | The engine logger. |

### Behaviour options

```cpp
registrar.addBehaviour<Turret>("turret", "Turret", fields,
                               BehaviourOptions{.runIn = RunIn::EditAndPlay, .category = "Combat"});
```

Behaviours run in Play only unless they opt into Edit, and appear under
**Add Component > Behaviours** unless you name another category.

## Rules that keep game code correct

These are the mistakes the engine is designed to catch; following them avoids
surprises.

1. **Move entities through `Scene`'s transform helpers.** Use `setPosition`,
   `setRotation`, `setScale`, `translate`, `setLocalTransform`,
   `setWorldTransform`; read with `localPosition`, `localRotation`,
   `localScale`, `worldTransform`, `worldPosition`. The setters mark the
   transform dirty so children and rendering update. Writing
   `TransformComponent` through `registry()` requires
   `scene.markTransformDirty(entity)` afterwards; the log warns if you forget.
2. **Create and destroy entities through commands, not directly.** Use
   `services->world().commands().spawn(prefab, placement, onSpawned)`,
   `spawnEmpty(name, ...)`, `destroy(id)`, and `reparent(child, parent)`.
   Commands apply after the current phase, so you never invalidate a view
   another system is iterating. Destroying through commands is also what
   triggers `onDestroy`.
3. **Hold `SceneEntityId`, not `Entity`, across frames.** `Entity` is an ECS
   handle that can be recycled; `SceneEntityId` is the stable 64-bit id that
   is saved in scenes. Convert with `scene.id(entity)` and
   `scene.findEntity(id)` (null if it is gone).
4. **Keep per-run state out of module statics.** Hot reload discards a
   module's statics. Keep state in components, in the scene registry's context
   (one value per scene, reset each Play), or in
   `services->content().resource<T>()` (one value per session). See
   [Per-game state](06-gameplay-services.md#per-game-state).
5. **Check nullable context pointers.** `services`, `actions`, `physics`,
   and `ui` are null in some modes (see the tables above).
6. **Let exceptions mean bugs.** A throwing hook or system is logged and
   disabled, and the editor stops Play. Do not use exceptions for control
   flow in game code.

## Finding other entities

```cpp
// All entities with a component (EnTT view):
for (const Entity e : scene.registry().view<HealthComponent, const TransformComponent>()) { ... }

// By tag (Project Settings > Tags):
for (const Entity e : findEntitiesWithTag(scene, "enemy")) { ... }   // engine/scene/Tags.hpp

// By name under a parent (e.g. inside a spawned prefab):
const Entity muzzle = scene.findDescendant(weapon, "Muzzle");

// By a reference field set in the Inspector:
const Entity target = scene.findEntity(turret.target);  // turret.target is a SceneEntityId field
```

## Iterating fast

1. Edit code in your IDE.
2. Press `Ctrl+B` in the editor (works from anywhere).
3. The module hot reloads; your scene keeps its component values.
4. `F5` to test, tune values live, **Apply to Edit Scene** what you want to
   keep, `Shift+F5`.

To see a complete game written this way, read the First-Person Shooter
template's `src/` (a hitscan weapon, targets, a goal zone, a HUD, and menus),
walked through in the [FPS tutorial](../tutorials/fps-game/README.md).

Deep dives: [Game modules](../engine/modules.md), [Scene](../engine/scene.md),
[Editor extensions](../EDITOR_EXTENSIONS.md) (add your own editor tools from
the module).

[Guide contents](README.md) · Previous: [Playing and debugging](04-play-and-debug.md) · Next: [Gameplay services](06-gameplay-services.md)
