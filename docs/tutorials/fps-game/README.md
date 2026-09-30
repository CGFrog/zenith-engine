# Build a first-person game with Zenith

This tutorial builds a small first-person target range using Zenith's current
C++20 APIs. At the end you will have:

- a separate game library and executable;
- a level authored in the Zenith editor and loaded as a scene document;
- a capsule-based player with mouse look, walking, sprinting, and jumping;
- a hitscan weapon driven by physics raycasts;
- gameplay state, a HUD, audio, and a reloadable scene; and
- CPU tests plus a bounded Vulkan smoke run.

The guide keeps reusable mechanics in `src/engine/` and FPS rules in a new
`src/fps/` client. Read [Architecture](../../ARCHITECTURE.md) before changing
those boundaries.

## Course outline

1. [Create the game target](01-create-the-game-target.md)
2. [Author and load the level](02-author-and-load-the-level.md)
3. [Build the first-person player](03-build-the-player.md)
4. [Add a hitscan weapon and targets](04-add-weapons-and-targets.md)
5. [Add HUD, audio, and game flow](05-add-hud-audio-and-game-flow.md)
6. [Test, profile, and package](06-test-profile-and-package.md)

## Prerequisites

- Windows with Visual Studio 2022 and the Desktop C++ workload
- a Vulkan 1.4-capable driver
- CMake available in PowerShell
- a Debug build of this repository

From the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 4
ctest --test-dir build -C Debug --output-on-failure
```

If these commands do not pass, fix the base checkout before starting the game.

## Use the demo as a live reference

The checked-in `zenith` executable already demonstrates the core loop this
tutorial develops:

| File | What to study |
| --- | --- |
| `src/game/application/DemoApplication.*` | Application startup, scene factory, and persistent music |
| `src/game/scene/GameplayScene.*` | Scene loading and variable/fixed updates |
| `src/game/player/FirstPersonController.*` | Mouse look, movement, grounding, and jumping |
| `src/game/scene/DemoScene.*` | Model instantiation, lighting, collision, and props |
| `tests/game/ApplicationLifecycleTests.cpp` | Headless gameplay and lifecycle tests |

You can first complete the tutorial by adapting `src/game/`. The chapters use
`src/fps/` so the architectural separation remains obvious and the original
demo stays available for comparison.

## Current scope

Zenith renders static and skinned glTF triangle geometry, skeletal animation,
and opaque, masked, or blended materials. Morph targets and compressed meshes
remain outside this tutorial's scope. The tutorial uses static environments,
primitive physics, and code-driven gameplay so its core remains approachable.
