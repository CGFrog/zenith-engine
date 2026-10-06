# Build a first-person game with Zenith

This tutorial makes a first-person target range the way any Zenith game is
made: a project created in the editor, gameplay written as C++ behaviours and
systems in the project's hot-reloaded game module, and settings edited in the
editor. At the end you will have:

- a project created from the First-Person Shooter template;
- a behaviour of your own, generated, built with Ctrl B, and added in the
  inspector;
- input actions, physics layers, and tags edited in Project Settings;
- a HUD, events between behaviours, and pause and settings menus;
- the game debugged in the editor and played with `zenith --project`.

The engine supplies the mechanisms: the character controller, physics
queries, events, effects, UI widgets, and menus. The game's rules live in its
module. [Architecture](../../ARCHITECTURE.md) explains that boundary.

## Course outline

1. [Create the project](01-create-the-project.md)
2. [Play and inspect the course](02-play-and-inspect.md)
3. [Write a behaviour](03-write-a-behaviour.md)
4. [Input, layers, and tags](04-input-layers-and-tags.md)
5. [Events, the HUD, and menus](05-events-hud-and-menus.md)
6. [Debug, test, and ship](06-debug-test-and-ship.md)

## Prerequisites

- Windows with Visual Studio 2022 (or 18) and the Desktop C++ workload, or
  Linux with GCC or Clang
- a Vulkan 1.4-capable driver
- CMake 3.25 or later
- a Debug build of this repository, with the editor:

```powershell
cmake --preset default
cmake --build --preset debug
```

Run the editor with no arguments to see the Project Hub:

```powershell
build/Debug/zenith_editor.exe
```
