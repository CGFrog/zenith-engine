# 1. Create the project

[Back to the outline](README.md)

## Create it from the template

In the Project Hub, under **New from Template**:

1. Choose **First-Person Shooter**.
2. Name the project, for example `Range Day`, and pick a folder.
3. Press **Create Project**. The hub opens it.

> 📷 **Screenshot needed — `TUT-FPS-01-TEMPLATE`:** *The Project Hub's New from Template section with First-Person Shooter selected and the name "Range Day" entered.*

The template is `templates/fps_game/` in the Zenith source tree. Its
`template.json` names the starter scene, the input action file, and the
project's tags, which the new project's `.zenith-project` file records
(manifest version 7, see [Projects](../../engine/projects.md)).

## Build the game module

Press **Ctrl B** (`build.run`). The Build panel, if open, streams the output;
the build also runs with no Build panel. The first build configures CMake
with the engine's compiler, so it takes a while. When it finishes, the editor
loads `modules/range_day.zmodule` and the status bar names the module.

Before the first build, the scene's module components (the weapon, targets,
and exit) are kept as data and shown as "(unknown type)" sections; the load
turns them into behaviours without losing their values.

> 📷 **Screenshot needed — `TUT-FPS-01-UNKNOWN`:** *An Inspector section shown as "(unknown type)" before the first build, and the same entity after the build with the Target behaviour section.*

**Build on save** (Preferences > Build) starts a build whenever you save a
source file in `src/`.

## What the project holds

| Path | Purpose |
| --- | --- |
| `range_day.zenith-project` | The manifest the editor and the game read. |
| `scenes/main.zenith-scene` | The course. |
| `input_actions.json` | The input actions; edited in chapter 4. |
| `assets/prefabs/impact.prefab` | The spark burst a shot leaves. |
| `src/GameModule.cpp` | The module's entry point: it registers every behaviour and system. |
| `src/FpsGame.*` | The round's state and events, the score system, the HUD and menus. |
| `src/HitscanWeapon.*`, `Target.*`, `GoalZone.*` | The behaviours. |

**Open in IDE** in the Project Hub, or any behaviour's **Edit Script**
button, opens the sources with `ide.command` (Preferences > External IDE).

Next: [Play and inspect the course](02-play-and-inspect.md).
