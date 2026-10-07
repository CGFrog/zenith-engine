# 4. Playing and debugging

[Guide contents](README.md) · Previous: [Building a level](03-building-levels.md) · Next: [Writing game logic](05-game-logic.md)

You can run your game inside the editor at any moment. Play always runs on a
**copy** of the scene: when you stop, the scene you were editing comes back
exactly as it was.

## Play, Simulate, Pause, Step, Stop

| Key | Toolbar | Operator | What happens |
| --- | --- | --- | --- |
| `F5` | Play | `play.start` | Runs the game with your input on the chosen play target. |
| `Alt+F5` | Simulate | `play.simulate` | Runs physics and fixed-update systems on a copy, keeping the editor camera and ignoring your input. Good for watching props settle. |
| `F6` | Pause | `play.toggle_pause` | Freezes the simulation. You can still inspect everything. |
| `F7` | Step | `play.step` | While paused, advances exactly one fixed time step. |
| `Shift+F5` | Stop | `play.stop` | Ends the run and restores the edited scene. |

The **playback toolbar** in the main menu bar has the same buttons, plus a
**time-scale** field that slows down or speeds up Play and Simulate.

While Play runs, the editor's headers and panes are tinted **warm**; while
Simulate runs, **violet**. That way you always know whether an edit will
last.

> 📷 **Screenshot needed — `GUIDE-04-PLAYTINT`:** *The editor during Play: warm
> tinted headers, the game view with the "Esc releases the cursor" banner, and
> the playback toolbar showing Stop, the play-target dropdown, Simulate, and
> the time-scale field.*

## Where the game runs: play targets

The arrow beside Play (or **Play > Play in ...**) chooses the target. `F5`
reuses your last choice.

| Target | Behaviour |
| --- | --- |
| **Active Viewport** (default) | The viewport you were using becomes the game view, looking through the scene's primary camera. Other viewports keep editing. Click the view to capture the mouse; `Esc` or a click elsewhere gives input back to the editor. |
| **Floating Viewport** | A new viewport floats over the editor, locked to the game camera. It closes when you stop. |
| **Standalone Game** | The scene, including unsaved changes, runs in a separate `zenith` process. Its output appears in the Info log under the `game` category. Stop ends the process. |

## Editing while the game runs

During Play or Simulate (also paused) you can select objects in the viewport
or the Hierarchy and edit them. The Inspector, Properties, the sidebar's Item
tab, and the gizmo show the **running** values every frame and change the
running scene immediately. Use this to tune numbers while you play.

These live edits:

- have **no undo**;
- are **lost on Stop**, as are objects you add or delete meanwhile (a banner
  reminds you).

To keep a change:

- right click a component header → **Apply to Edit Scene** (that one
  component), or
- **Apply Selection to Edit Scene** (Inspector banner, Hierarchy right click,
  or the **Play** menu) for the whole object: name, transform, and every
  component. An object spawned while running is created in the edit scene.

Each apply is one undo step on the edited scene. Saving, prefab and
collection commands stay disabled until you stop, and say so in the Info log.

> 📷 **Screenshot needed — `GUIDE-04-LIVEEDIT`:** *The Inspector during Play with
> the live-edit banner, and the component header context menu open on "Apply
> to Edit Scene".*

## When the game throws

Game code cannot crash the editor through a C++ exception. If a system or
behaviour throws:

- the error is logged in the `gameplay` category with the system or behaviour
  name, the entity, the hook, and the exception message;
- that system or behaviour is disabled;
- in the editor, **Play stops** and the edit scene is restored, so you can fix
  the code, press `Ctrl+B`, and play again.

The System Profiler shows a disabled system as `<name> (disabled)` with the
reason in its tooltip. A hard crash (access violation) goes to the crash
handler, which saves your open document to the recovery file first.

## Debugging tools

### The Info log and log categories

Log from game code through a named channel:

```cpp
context.services->log("weapons").info("Hit for " + std::to_string(damage));
```

Its category is `gameplay.weapons`. The Info panel's **Categories** dropdown
shows or hides each category, so you can mute noisy parts of the game.

> 📷 **Screenshot needed — `GUIDE-04-INFOLOG`:** *The Info panel with the
> Categories dropdown open, gameplay.weapons ticked and gameplay.ai unticked.*

### Watched fields

During Play, right click any field in the Inspector → **Watch Field**. The
Properties panel shows a **Watched** section above every tab with the value as
the game changes it. Remove one with its `x`, or all with **Module > Clear
Watched Fields**. Watches survive Stop, so they show again next Play.

> 📷 **Screenshot needed — `GUIDE-04-WATCH`:** *The Properties panel's Watched
> section showing two watched fields (e.g. a target's health and the player's
> velocity) updating during Play.*

### Debug drawing

`OutsideServices::debugDraw()` draws lines, spheres, boxes, and labels in every
viewport, the game view, and the standalone game:

```cpp
DebugDraw& draw = context.services->outside().debugDraw();
draw.line(origin, origin + forward * range, {.color = {1.0f, 0.2f, 0.2f, 1.0f}});   // one frame
draw.channel("weapons.hits").forSeconds(2.0f).sphere(hitPoint, 0.1f);              // two seconds
draw.channel("weapons.spawns").persistent().label(spawnPoint, "spawn");          // until cleared
draw.clearChannel("weapons.spawns");
```

The viewport's **Overlays > Debug Draw** section lists the channels so you can
hide one. Systems registered to run in Edit mode can draw too, for previews.

> 📷 **Screenshot needed — `GUIDE-04-DEBUGDRAW`:** *The FPS template during Play
> with red hitscan rays and hit spheres drawn, and the Overlays menu open on
> its Debug Draw channel list.*

### The System Profiler

In the **Debug** workspace, the System Profiler lists each module system's
phase, modes, and frame / average / maximum CPU time, the engine's CPU phases,
and the GPU pass table. Its checkboxes choose which systems Simulate runs.

> 📷 **Screenshot needed — `GUIDE-04-PROFILER`:** *The System Profiler during
> Play on the FPS template, listing the score and hud systems and the built-in
> zenith.* systems with their timings.*

### A real debugger

**Module > Attach Debugger** runs the debugger command from **Preferences >
External IDE** (on Windows `vsjitdebugger.exe -p {pid}` by default) with the
editor's process id. Breakpoints in your module's code then hit: the editor
loads a copy of the module together with its symbols.

## Hot reload

Every time you build (`Ctrl+B`), the editor reloads your module:

- In Edit mode the new code loads at once. Components keep their values,
  because they are reloaded through their saved fields.
- During Play, the status bar says **Reload pending - stop to apply**; the new
  code loads on Stop.
- If the build fails, the old module keeps running and behaviour sections show
  a **failed** badge. Compiler errors appear in the Build and Diagnostics
  panels; click one to open your IDE at that line.
- A module that fails to load keeps its components' data as JSON, so nothing
  in your scene is lost; the next successful build restores them.

> 📷 **Screenshot needed — `GUIDE-04-BUILDERRORS`:** *The Build panel showing a
> compiler error, and the same error in Diagnostics, with a behaviour section
> in the Inspector showing its "failed" badge.*

[Guide contents](README.md) · Previous: [Building a level](03-building-levels.md) · Next: [Writing game logic](05-game-logic.md)
