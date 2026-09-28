# 2. Author and load the level

Keep source assets and authored scenes under `assets/` so the existing
`zenith_assets` target copies both into `build/assets/`:

```text
assets/
|- models/training_room.glb
|- models/target.glb
|- audio/
`- scenes/training.zenith-scene
```

## Create a project file

At the repository root, create `fps.zenith-project`:

```json
{
  "version": 1,
  "assetRoot": "assets",
  "sceneRoot": "assets/scenes"
}
```

Launch the editor with the project:

```powershell
build\Debug\zenith_editor.exe fps.zenith-project
```

In the editor:

1. Choose **File > New**.
2. Drag the room model from Asset Browser into the viewport.
3. Add one primary directional light and one environment light.
4. Add static landmarks and spawn markers as named entities.
5. Save as `assets/scenes/training.zenith-scene`.

Do not leave an authored camera marked primary for this level; the runtime
player camera created in the next chapter should be the primary view.

The general editor workflow is covered in
[Author a first Zenith level](../../EDITOR_TUTORIAL.md). Save paths inside scene
documents are relative to the project asset root, so moving the repository does
not invalidate model references.

## Load the scene document

Implement the scene lifecycle in `TrainingScene`:

```cpp
// src/fps/scene/TrainingScene.hpp
#pragma once

#include "engine/application/Application.hpp"
#include "fps/player/FpsController.hpp"
#include "fps/weapons/HitscanWeapon.hpp"

class TrainingScene final : public ApplicationScene {
  public:
    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

  private:
    FpsControllerSystem m_controllers;
    HitscanWeaponSystem m_weapons;
};
```

```cpp
SceneResult TrainingScene::load(ApplicationContext& context) {
    const std::filesystem::path assetRoot{ZENITH_ASSET_DIR};
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    const auto result = SceneDocument::load(assetRoot / "scenes" / "training.zenith-scene",
                                            context.assets, options, context.scene);
    if (!result) {
        return {.error = "Could not load training level: " + result.error};
    }

    // The next chapter creates the player here.
    return {.success = true};
}
```

Include `engine/scene/SceneDocument.hpp` and `<filesystem>` in the `.cpp`.
Loading is transactional: malformed input does not partially replace the live
scene, and returning the diagnostic causes application startup to fail cleanly.

## Add collision

Visual geometry does not automatically become physical. For a simple training
room, locate the imported room root or its mesh entities and attach static
collision. Triangle-mesh colliders are supported only for static bodies:

```cpp
registry.emplace<ColliderComponent>(entity, TriangleMeshCollider{meshRenderer.mesh});
registry.emplace<RigidBodyComponent>(entity, RigidBodyComponent{
    .motionType = PhysicsMotionType::Static,
    .layer = PhysicsLayer::Static,
});
```

For floors and walls, primitive box colliders are cheaper and easier to tune.
Use multiple invisible entities with `BoxCollider` when the rendered room is
complex. The transform scale is included when Zenith creates the physics shape.

## Choose a spawn marker

The scene format persists names, so a small game-side search is enough for a
first spawn convention:

```cpp
Entity findNamedEntity(const Scene& scene, std::string_view name) {
    auto names = scene.registry().view<const NameComponent>();
    for (Entity entity : names) {
        if (names.get<const NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}
```

Name an editor entity `PlayerSpawn`. If it is missing, return a descriptive
load error rather than silently spawning at an arbitrary position.
