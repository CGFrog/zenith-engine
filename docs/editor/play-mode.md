# Play, Simulate, and play targets

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Simulate (`play.simulate`, `Alt+F5`) is `EditorMode::Simulate`: like Play it
runs on a snapshot that Stop restores (and `Paused` covers both;
`EditorPlayMode::simulating()` tells which), but systems run in
`SimulationMode::Simulate` with no input (`GameModuleHost::runSystems` drops it)
and only the systems the Simulate mask enables, while the editor keeps its
camera. The mask is edited with `play.set_simulate_system {system, enabled}`
(the System Profiler's checkboxes and the shared Simulate system picker,
`panels/SimulateSystemPicker`, shown in the Play menu, the profiler's
**Simulate Systems** popover, and available to the playback toolbar as
`drawSimulateSystemPickerButton`), kept across module reloads, and saved to
`simulate-systems.json` (`settings/SimulateSystemSettings`).
`play.set_time_scale {scale}` sets `EditorPlayMode`'s time scale, which the
session's `simulationClock()` reports in Play and Simulate (real time in Edit)
and which also scales Update and LateUpdate systems. The session applies the
game module's own pause and time scale on top (`GameServices::
simulationClock`), so a game that pauses itself for a menu stops in the
editor as it does in a build. The **System Profiler** area
(`panels/SystemProfilerPanel`, type `system_profiler`) shows each module
system's phase, modes, and frame/average/max CPU time (`GameModuleHost::
systemProfile()`, published by `finishSystemFrame` at the start of `drawUi`),
the engine's CPU phases, and the GPU pass table; `profiler.reset_max` restarts
the maxima. The headless play operators live in `operators/PlayOperators`.

Play targets (`application/PlayTargets`, wired in `application/
EditorPlayback.cpp`): `play.start {target}` runs Play in `viewport` (the
active viewport), `floating` (a new Viewport area undocked through
`AreaInstance::floatRequest`, closed on Stop), or `standalone`; without a
target (`F5`) it reuses the last one. A viewport target turns the viewport
into a game view (`ViewportPanel::beginGameView`): it looks through
`gameCamera(scene)` (the first primary camera, the renderer's fallback) by
clearing its view override, ignores editor navigation, picking, and the
gizmo, and shows a banner about game input. Module systems get the frame's
`InputState` only while the game view has focus (`EditorSession::gameInput`;
a neutral state otherwise), and `EditorSession::mouseCapturePolicy()` is
`Persistent` exactly then, published to `EditorApplication` each frame.
`play.focus_game_view` (a click on the view) and `play.release_game_input`
(Esc or a click elsewhere) move the focus. `standalone` writes the document
to a temporary scene and runs `zenith [--project <file>] --scene <scene>`
through `StandalonePlayer` (its `GameProcess` is a `ChildProcess`, a fake in
tests), streams the output into the log under `game`, removes the scene when
the game ends, and leaves Play then; Stop cancels it. `play.step` (`F7`)
advances a paused runtime scene by one frame of the fixed time step
(`EditorPlayMode::requestFrameStep`, the clock's `frameStepSeconds`).
`ThemeChrome::setModeTint` washes the editor warm in Play and violet in
Simulate (`playModeTint`). The playback toolbar after the Module menu holds
the play buttons, the target dropdown, a Simulate dropdown whose contents are
`EditorSession::drawSimulateOptions` (the Simulate system picker's slot), and
the time-scale field. `tests/editor/PlayTargetsTests.cpp` and
`EditorSessionTests` cover the targets headless; the editor smoke plays the
startup scene in the active viewport and, when `zenith` is built, launches it
standalone for a moment (its `play targets` step, see [Editor smoke](smoke.md)).

## Debugging the game

- **Debug draw.** Gameplay `DebugDraw` (`GameServices`) lines and labels show
  in every view, game views included; editor gizmos and their labels only
  where `viewportDrawsSceneGizmos` allows.
- **Watched fields.** A reflected field's context menu (right click) has
  **Watch Field** (`debug.watch_field {entity, component, field, label}`,
  `operators/DebugOperators`), which adds it to the session's
  `interaction/WatchList`. The Properties panel shows a **Watched** section
  above every tab with each field's value in the active scene, read every
  frame through `watchedValueText` (runtime-state fields included): the
  running scene's in Play and Simulate. The list is editor state (no undo,
  not saved) and keeps watches on entities that Stop removes, so they show
  again in the next Play. `debug.unwatch_field` (the row's `x`) and
  `debug.clear_watches` (Module menu) remove them.
- **Attach Debugger** (`debug.attach`, Module menu) runs `ide.debug_command`
  (Preferences > External IDE; `vsjitdebugger.exe -p {pid}` by default on
  Windows) with the editor's process id (`currentProcessId`), so breakpoints
  in the game module's shadow-copied code hit. `expandDebuggerCommand` shares
  `ExternalEditor`'s tokenizing with `{pid}` as its only placeholder.
