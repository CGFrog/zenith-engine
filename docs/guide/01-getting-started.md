# 1. Getting started

[Guide contents](README.md) · Next: [The editor at a glance](02-editor-tour.md)

By the end of this chapter you will have built Zenith, opened the editor, and
created a project of your own that you can play.

## What you need

| Requirement | Notes |
| --- | --- |
| A C++20 compiler | Visual Studio 2022 (or newer) with the *Desktop development with C++* workload on Windows; GCC or Clang on Linux. |
| CMake 3.25 or newer | The build and every game project use CMake presets. |
| Git and Python 3 | CMake downloads the pinned third-party libraries on first configure. |
| A Vulkan 1.4 GPU and driver | Needed to run the editor and games. CPU-only tests run without one. |
| `clang-format` 19+ (optional) | Only if you will contribute engine code. |

The first configure downloads dependencies into `build/_deps` and needs
network access. Later configures are offline.

## Build the engine and editor

From the repository root:

```powershell
cmake --preset default        # configure build/ (Debug)
cmake --build --preset debug  # build engine, editor, game runner, tests
```

`cmake -S . -B build` followed by `cmake --build build --config Debug` does
the same thing without presets.

A successful build gives you, in `build/Debug/` (Visual Studio) or `build/`
(single-config generators such as Ninja):

| Program | What it is |
| --- | --- |
| `zenith_editor` | The editor. This is where you will spend most of your time. |
| `zenith` | The game runner. It plays a project outside the editor: `zenith --project <file>`. |
| `zenith_asset_build` | Command-line asset cooker (textures, derived data). |
| `zenith_engine` (`.dll`/`.so`) | The shared engine library both programs load. |

The build also compiles every shader under `shaders/` to SPIR-V and copies
`assets/` and `templates/` next to the programs, so they run from any working
directory.

> **Tip.** Building is the slowest part of the first day. Precompiled headers
> and parallel compilation are on by default; see
> [Development › Build speed](../DEVELOPMENT.md#build-speed) for the optional
> `sccache` compiler cache.

## Open the editor

```powershell
build/Debug/zenith_editor.exe
```

Started without arguments, the editor opens the **Project Hub**.

> 📷 **Screenshot needed — `GUIDE-01-HUB`:** *The Project Hub on first launch:
> the Recent Projects list (empty or with the demo), the New from Template
> section showing both templates, and the Open Project... button.*

The hub has four jobs:

- **Recent Projects** lists projects you have opened, newest first. **Open**
  opens one. A project whose file has disappeared is marked *(missing)*;
  **Remove** only drops it from the list and never touches its files.
- **New from Template** creates a project in a new folder named after it.
- **Open Project...** opens any `.zenith-project` file.
- **Open in IDE** opens a project's folder in your IDE (the command is set in
  **Edit > Preferences... > External IDE**).

To skip the hub, pass a project file:

```powershell
build/Debug/zenith_editor.exe projects/demo.zenith-project
```

## Try the bundled projects

The repository ships with projects under `projects/` that are useful to
explore before you make your own:

| Project | What it shows |
| --- | --- |
| `demo.zenith-project` | A small room with a physics player and five pickups, played through the demo game module. |
| `showcase.zenith-project` | The renderer showcase: PBR materials, lighting, shadows, post-processing. |
| `irradiance_room.zenith-project` | An interior lit by a baked irradiance volume. |
| `material_assets.zenith-project` | Material (`.zmat`) assets and overrides. |

Open the demo, press `F5` to play, walk with `WASD`, and press `Shift+F5` to
stop.

## Create your first project

In the Project Hub choose **New from Template** and pick one:

| Template | You get |
| --- | --- |
| **C++ Game** | A project file, `assets/`, `scenes/main.zenith-scene` (ground, cube, sun, sky, camera), and `src/` with a CMake game module that holds an example component (**Spinner**) and system. Start here for a new game. |
| **First-Person Shooter** | A playable target range: a first-person player, a hitscan weapon, targets, a goal zone, a HUD, and pause and settings menus. The [FPS tutorial](../tutorials/fps-game/README.md) walks through it. |

> 📷 **Screenshot needed — `GUIDE-01-NEWPROJECT`:** *The New from Template form
> with C++ Game selected, a project name typed in, and the location field with
> its Browse... button.*

The new project opens straight away. Its folder looks like this:

```text
my_game/
|- my_game.zenith-project   project manifest: name, folders, startup scene, render settings, module
|- assets/                  models, textures, audio, prefabs, materials (scenes refer to these by path)
|- scenes/main.zenith-scene the startup scene
|- src/                     your game module (C++): GameModule.cpp + your types
|- CMakeLists.txt           builds the module against the Zenith build at ZENITH_ROOT
|- CMakePresets.json        the "zenith" preset the editor's Build panel runs
`- modules/                 where each build drops my_game.zmodule (generated)
```

## Build the game module and see it run

Your game's code lives in a **game module**: a shared library the editor loads
and hot reloads. Build it once:

1. Press `Ctrl+B` (or **Build** in the Build panel). The first build
   configures the project and takes a little while; later builds only rebuild
   what changed.
2. When the build finishes, the editor loads the module. Its component types
   now appear in **Add Component** menus.
3. Select the cube, choose **Add Component > Gameplay > Spinner**, and press
   `F5`. The cube turns. Press `Shift+F5` to stop.

> 📷 **Screenshot needed — `GUIDE-01-SPINNER`:** *The C++ Game template scene in
> Play mode with the cube mid-rotation, the Inspector showing the Spinner
> section (degrees_per_second = 90), and the warm Play tint on the panel
> headers.*

The project builds against the Zenith build the editor runs from. A module
loads only into an editor built with the same compiler and runtime, so keep
the two in step; the project's own `README.md` explains `ZENITH_ROOT`.

## Where your files live

| What | Where |
| --- | --- |
| Your project | Wherever you created it. Everything it needs is inside its folder. |
| Editor settings (keymap, workspaces, preferences, recent projects) | The editor settings folder in your user data directory. Set `ZENITH_EDITOR_SETTINGS_DIR` to use a scratch folder instead. |
| Unsaved-scene recovery copies | Written periodically while a scene has unsaved changes and offered after a crash. Interval in **Preferences > Saving**. |

[Guide contents](README.md) · Next: [The editor at a glance](02-editor-tour.md)
