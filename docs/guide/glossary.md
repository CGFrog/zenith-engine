# Glossary

[Guide contents](README.md)

| Term | Meaning | Where it lives |
| --- | --- | --- |
| **Action** (input action) | A named button or axis (`Jump`, `Move`) bound to device inputs. Game code reads actions, never keys. | `engine/input/InputActions.hpp`, `input_actions.json` |
| **Active viewport** | The viewport the mouse last hovered or clicked; keys and the status bar act on it. | `editor/panels/ViewportRegistry` |
| **Adjust Last Operation** | The bottom-left viewport panel that re-runs the last add/duplicate/array with new values. | editor |
| **Apply to Edit Scene** | Copies a live Play-mode change back into the edited scene as one undo step. | `play.apply_to_edit` |
| **Area** | A dockable editor window whose type (Viewport, Inspector, ...) can be switched. | `editor/areas/` |
| **Asset** | A file under the project's asset root: model, texture, audio, prefab, material, data table. | `engine/assets/` |
| **Asset GUID / `.meta`** | The permanent id of an asset, stored in a sidecar file, so references survive renames. | `engine/assets/AssetDatabase` |
| **Behaviour** | A component class with lifecycle hooks (`onStart`, `onUpdate`, ...). Per-entity logic. | `engine/behaviour/Behaviour.hpp` |
| **Character controller** | A walking capsule moved by physics without a rigid body (`CharacterVirtual`). | `engine/physics/CharacterController.hpp` |
| **Collection** | A named, nestable group of objects, separate from parenting. Can be hidden, unselectable, or excluded from rendering. | `engine/scene/` |
| **Collection instance** | An empty that draws a collection's objects at its own transform. | `collection_instance` |
| **Command (entity command)** | A deferred spawn/destroy/reparent recorded during a phase and applied after it. | `engine/module/EntityCommandBuffer.hpp` |
| **Component** | A plain data struct attached to an entity, described by `Fields<T>`. | `engine/scene/ComponentTypes.hpp` |
| **Component key** | The string a component is saved under in scene files (`"rigid_body"`). Never change it once scenes use it. | |
| **Definition table** | A strict JSON array of reflected entries (items, enemies) loaded by id. | `engine/gameplay/DefinitionTable.hpp` |
| **Diagnostics** | Problems with suggested actions: import failures, compiler errors, missing references. | editor |
| **Edit mode** | The editor's normal state: changes are authored, saved, and undoable. | |
| **Effect** | A prefab spawned with an automatic lifetime (and optional pooling). | `engine/module/Effects.hpp` |
| **Entity** | An object in a scene; an id that components attach to. `Entity` is the transient ECS handle. | EnTT |
| **Event** | A struct with a unique `eventKey`, published in frame N and read in frame N + 1. | `engine/module/EventQueue.hpp` |
| **Fields<T>** | The explicit reflection description of a struct's fields; drives saving, the Inspector, undo, prefabs. | `engine/reflection/Reflection.hpp` |
| **Fixed step** | The constant physics/FixedUpdate interval (1/60 s by default). | `engine/application/` |
| **GameServices** | The per-session service hub: commands, events, timers, effects, audio, random, resources, logs. | `engine/module/GameServices.hpp` |
| **Game module** | Your game's shared library (`.zmodule`), loaded and hot reloaded by the editor and the game runner. | `engine/module/` |
| **Gameplay** | The component types and systems a game plays, from a module or linked in. | `engine/module/Gameplay.hpp` |
| **Hot reload** | Rebuilding and reloading a module while the editor runs, keeping component values. | `editor/modules/` |
| **Leases** | References that keep an asset alive while in use; unleased assets can be collected. | `engine/assets/` |
| **Live edit** | Editing the running scene during Play; not undoable and lost on Stop unless applied. | |
| **Operator** | A named editor command (`object.duplicate`) that menus, keys, and search run. | `editor/operators/` |
| **Override** (prefab) | A field value on an instance that differs from its prefab. | `engine/scene/PrefabOverrides.cpp` |
| **Phase** | When a system runs in a frame: `Update`, `FixedUpdate`, `LateUpdate`, `Ui`. | `engine/module/SystemScheduler.hpp` |
| **Play** | Running the game in the editor on a copy of the scene. | `play.start` |
| **Play target** | Where Play runs: active viewport, floating viewport, or standalone game process. | `editor/application/PlayTargets` |
| **Prefab** | A saved, reusable group of objects (`.prefab`) whose instances update when it changes. | `engine/scene/Prefab*` |
| **Prefab window** | A floating editor window that edits one prefab beside the open level. | `editor/application/PrefabWindowSet` |
| **Project** | A `.zenith-project` manifest plus its folders: assets, scenes, module, settings. | `engine/project/` |
| **Project Hub** | The editor's start screen: recent projects and templates. | `editor/project/` |
| **Project Settings** | Project-wide input actions, physics layers, tags, and navigation settings. | `project.settings` |
| **Render settings** | Project-wide rendering options (AA, bloom, AO, SSR, exposure, ...). | `engine/graphics/Rendering.hpp` (`RenderSettings`) |
| **RunIn** | Whether a system or behaviour runs in Edit, Play, or both. | `engine/module/SystemScheduler.hpp` |
| **Scene** | A document of entities (`.zenith-scene`), and the runtime ECS registry holding them. | `engine/scene/Scene.hpp` |
| **SceneEntityId** | A stable 64-bit entity id, saved in scenes; use it to remember entities. | `engine/scene/SceneEntityId.hpp` |
| **Sensor** | A rigid body that reports trigger enter/exit instead of colliding. | `rigid_body.sensor` |
| **Session resource** | One value per type kept for the whole Play session (`services.resource<T>()`). | `engine/module/SessionResources` |
| **Shadow copy** | The temporary copy of a module the editor loads, so the original can be rebuilt. | `engine/module/GameModule` |
| **Simulate** | Running physics and FixedUpdate systems on a copy of the scene without player input. | `play.simulate` |
| **System** | A function run once per phase over the scene. | `engine/module/SystemScheduler.hpp` |
| **Tag** | A project-declared label on an entity, found with `findEntitiesWithTag`. | `engine/scene/Tags.hpp` |
| **Template** | A starting point for new projects (C++ Game, First-Person Shooter). | `templates/` |
| **Timer** | A callback after an amount of game time, optionally repeating and owned by an entity. | `engine/module/Timers.hpp` |
| **Transient entity** | Generated content that is never saved (e.g. a procedural level preview). | `TransientComponent` |
| **Workspace** | A named layout of areas: Layout, Lighting, Debug. | `editor/areas/` |
| **3D cursor** | The red-and-white ring where new objects appear and an optional pivot point. | `editor/viewport/Cursor3D` |
