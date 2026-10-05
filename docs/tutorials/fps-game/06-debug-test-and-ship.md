# 6. Debug, test, and ship

[Back to the outline](README.md)

## Watch values while playing

Press F5, select a target, right click its **Health** field, and choose
**Watch Field**. The Properties panel shows a **Watched** section above
every tab with the value as the game changes it. Remove a watch with its
`x`, or all of them with **Module > Clear Watched Fields**.

## Log by category

Log from game code through a named channel:

```cpp
context.services->log("weapons").info("Hit " + std::to_string(damage));
```

Its category is `gameplay.weapons`. The Info panel's **Categories** dropdown
shows or hides each category, so noisy parts of the game can be muted.

## Draw debug shapes

`GameServices::debugDraw()` draws shapes and labels for one frame. They show
in every viewport, in the editor's game view, and in the standalone game
(`zenith --project`). For example, draw the weapon's ray:

```cpp
context.services->debugDraw().line(origin, origin + forward * range, {.color = {1.0f, 0.2f, 0.2f, 1.0f}});
```

To keep a shape on screen longer, give it a duration in seconds or keep it
until you clear it, and file it in a named channel. The viewport's
**Overlays** menu lists the channels under **Debug Draw**, so you can hide
one while you look at something else:

```cpp
DebugDraw& draw = context.services->debugDraw();
draw.channel("weapons.hits").forSeconds(2.0f).sphere(hitPoint, 0.1f);
draw.channel("weapons.spawns").persistent().label(spawnPoint, "spawn");
draw.clearChannel("weapons.spawns");
```

## Attach a debugger

**Module > Attach Debugger** (`debug.attach`) runs `ide.debug_command`
(Preferences > External IDE; on Windows `vsjitdebugger.exe -p {pid}`) with
the editor's process id. Breakpoints in the module's sources then hit; the
editor loads a shadow copy of the module with its symbols.

## Test without a window

The Zenith repository builds this template's module in a test
(`zenith_fps_template_module_build`) and plays its scene headless: the test
fires at a target and checks the HUD counts it. A game's own tests can do
the same with `GameplayRunner` over its module: load the scene, step physics,
and read `GameUiContext::commands()`.

## Play outside the editor

```powershell
build/Debug/zenith.exe --project "C:/Users/you/Documents/Zenith Projects/range_day/range_day.zenith-project"
```

The game loads the same module, applies the project's input actions, and
draws the same HUD. **File > Build and Package** builds a release package of
the project for this computer's platform.

[Back to the outline](README.md)
