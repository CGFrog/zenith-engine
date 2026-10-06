# Cookbook

[Guide contents](README.md)

Short answers to "how do I...?". Each recipe links to the chapter that
explains it.

## Editor

| How do I... | Do this |
| --- | --- |
| find a command I can't locate? | `F3` (or `Space`) and type part of its name. |
| frame the selected object? | `Numpad .` (or **View > Frame Selected**). `Home` frames everything. |
| move an object exactly 2 m up? | Select it, `G`, `Z`, type `2`, `Enter`. |
| place objects on a surface? | Snap To **Surface** (Gizmos menu), or drag a model from the Asset Browser onto the surface; tick **Align to Normal** in Adjust Last Operation. |
| make 10 copies in a row? | **Object > Array**, then set the count in Adjust Last Operation. |
| scatter rocks over terrain? | Select a rock, **Object > Scatter**; change count and seed in Adjust Last Operation. |
| reuse a set of objects? | **Object > Prefab > Create from Selection**, then drag the `.prefab` in. |
| edit a prefab without leaving my level? | Double-click the `.prefab`: it opens in its own window over the level. |
| push an instance's changes into its prefab? | **Object > Prefab > Apply**. |
| hide something from the game, not just the editor? | Turn off **Renderable** on its **Visibility** component or collection. (`H` only hides in the editor.) |
| look through the game camera? | `Numpad 0`. Any view key returns. |
| see colliders, light ranges, or names? | Viewport **Overlays** menu: set the category to Selected Only or All. |
| edit two objects' lights at once? | Select both; edit the shared field (differences show `—`). |
| copy one component to other objects? | Right click its header > **Copy to Selected**. |
| change a shortcut? | **Edit > Keymap...**, click the shortcut, press the new chord. |
| get Blender-style pie menus? | **Preferences > Interface > Pie menus**. |
| reset a messed-up layout? | **View > Reset Workspace**. |
| run the editor with clean settings? | Set `ZENITH_EDITOR_SETTINGS_DIR` to an empty folder. |

## Play and debug

| How do I... | Do this |
| --- | --- |
| keep a value I tuned during Play? | Right click the component header > **Apply to Edit Scene**. |
| watch a value change while playing? | Right click the field > **Watch Field**; see Properties > Watched. |
| slow the game down to look at something? | Change the time-scale field on the playback toolbar, or `F6` then `F7` to step. |
| see why my system stopped running? | Info log (category `gameplay`) and the System Profiler tooltip on `<name> (disabled)`. |
| debug my module in Visual Studio? | **Module > Attach Debugger**, set breakpoints, press `F5`. |
| test the real game process? | Play target **Standalone Game**, or `zenith --project <file>`. |

## Game code

### Rotate something every frame

```cpp
void spin(const SystemContext& context) {
    auto spinners = context.scene.registry().view<const SpinnerComponent>();
    for (const Entity entity : spinners) {
        const float degrees = spinners.get<const SpinnerComponent>(entity).degreesPerSecond * context.deltaSeconds;
        const glm::quat turn = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
        context.scene.setRotation(entity, context.scene.localRotation(entity) * turn);
    }
}
```

([chapter 5](05-game-logic.md#systems-logic-over-the-whole-scene))

### Get the direction an entity faces

```cpp
const glm::mat4 world = scene.worldTransform(entity);
const glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));   // -Z is forward
const glm::vec3 right   =  glm::normalize(glm::vec3(world[0]));
const glm::vec3 up      =  glm::normalize(glm::vec3(world[1]));
const glm::vec3 position(world[3]);
```

### Shoot a ray from the camera

```cpp
PhysicsRayHit hit;
const std::array<Entity, 1> ignored{shooter};
if (context.physics->raycast(position, forward, 100.0f, hit, {.ignore = ignored})) {
    context.services->debugDraw().line(position, hit.position);
}
```

([chapter 6](06-gameplay-services.md#physics-queries))

### Spawn a prefab where something was hit

```cpp
context.services->commands().spawn(
    "prefabs/crate.prefab", SpawnPlacement{.localTransform = glm::translate(glm::mat4(1.0f), hit.position)});
```

### Destroy myself after 5 seconds

```cpp
void Projectile::onStart(BehaviourContext& context) {
    const SceneEntityId self = context.scene.id(context.entity);
    GameServices* services = context.services;
    services->timers().start(5.0f, TimerRepeat::Once,
                             [services, self](TimerContext&) { services->commands().destroy(self); }, self);
}
```

(The timer is owned by `self`, so it is dropped if the projectile is destroyed
earlier.)

### React when the player enters an area

Give the area a rigid body with **Sensor** ticked and a collider, then:

```cpp
void Checkpoint::onTriggerEnter(BehaviourContext& context, Entity other) {
    if (context.services != nullptr && context.scene.registry().all_of<CharacterControllerComponent>(other)) {
        context.services->events().publish(CheckpointReached{.checkpoint = context.scene.id(context.entity)});
    }
}
```

### Tell the score system something happened

```cpp
struct EnemyKilled {
    static constexpr std::string_view eventKey = "my_game.enemy_killed";
    int points{0};
};
services.events().publish(EnemyKilled{.points = 100});                 // publisher
for (const EnemyKilled& e : services.events().read<EnemyKilled>()) {   // reader, next frame
    score += e.points;
}
```

### Keep score for the current round

```cpp
struct RoundState { int score{0}; };

RoundState& roundState(Scene& scene) {
    auto& context = scene.registry().ctx();
    if (RoundState* state = context.find<RoundState>()) {
        return *state;
    }
    return context.emplace<RoundState>();
}
```

### Point an entity at a target

```cpp
const glm::vec3 from = scene.worldPosition(turret);
const glm::vec3 to = scene.worldPosition(target);
// quatLookAt expects the direction to look along; Zenith looks down -Z, as GLM assumes.
scene.setRotation(turret, glm::quatLookAt(glm::normalize(to - from), glm::vec3(0.0f, 1.0f, 0.0f)));
```

(`setRotation` sets the *local* rotation; this is correct for a root entity.
For a child, convert by the parent's world rotation first.)

### Find all entities with a tag

```cpp
#include "engine/scene/Tags.hpp"
for (const Entity enemy : findEntitiesWithTag(scene, "enemy")) { ... }
```

### Make an AI walk somewhere

```cpp
auto& agent = scene.registry().get<NavAgentComponent>(entity);
setNavDestination(agent, targetPosition);   // engine/navigation/NavAgent.hpp
// later: agent.arrived
```

### Draw a health bar

```cpp
ui.progressBar({{24.0f, -48.0f}, {320.0f, 20.0f}}, health.current / health.maximum,
               {0.9f, 0.2f, 0.2f, 1.0f}, {.color = {0, 0, 0, 0.6f}, .cornerRadius = 4.0f}, UiAnchor::BottomLeft);
```

### Pause the game with a menu

```cpp
if (context.actions != nullptr && context.actions->pressed("Pause")) {
    paused = !paused;
    context.services->setPaused(paused);
}
// In the Ui system:
if (paused) {
    switch (drawPauseMenu(*context.ui)) {
    case MenuAction::Resume: paused = false; context.services->setPaused(false); break;
    case MenuAction::Quit:   /* request quit */ break;
    default: break;
    }
}
```

### Play a sound at a position

```cpp
if (AudioSystem* audio = services.audio()) {
    if (const auto clip = services.loadClip("audio/explosion.ogg")) {
        AudioSpatialOptions options;
        options.position = where;
        audio->playAt(clip.handle, options);
    }
}
```

### Roll random loot reproducibly

```cpp
RandomStream& loot = services.random("loot");
const size_t index = loot.weightedIndex(dropWeights);
```

### Add a tool to the editor from my module

```cpp
#include "editor/modules/EditorModuleApi.hpp"
#include <imgui.h>

ZENITH_EDITOR_MODULE_REGISTER {
    registrar.addSidebarTab({.title = "My Game", .draw = [](EditorModulePanelContext& context) {
        ImGui::Text("%zu selected", context.selection.size());
        if (ImGui::Button("Duplicate")) {
            context.invokeOperator("object.duplicate");
        }
    }});
}
```

See [Editor extensions](../EDITOR_EXTENSIONS.md) for operators, panels,
inspectors, and menus.
