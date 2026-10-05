# Editor smoke

`ZENITH_EDITOR_SMOKE=1` makes the editor run a scripted scenario against the
scene it opened and quit when it finishes. The CTests
`zenith_editor_vulkan_smoke` (the default scene) and
`zenith_editor_module_smoke` (the demo scene with a game module, which the
scenario also hot reloads) run it under Vulkan; see
`tests/editor/CMakeLists.txt`. Automated runs also set
`ZENITH_EDITOR_SETTINGS_DIR`, and `ZENITH_SMOKE_FRAMES` only as a safety cap
above the sum of the step budgets.

## How it runs

The scenario is a list of steps in `src/editor/smoke/`:

- `SmokeRunner` (`SmokeRunner.hpp`) runs steps strictly one after another.
  Each frame it calls the active step's `update` with a step frame that counts
  from 0 when the step became active. A step returns `Running`, `Done` (the
  next step starts on the next frame), or `Failed` with a message. A step
  still running after `frameBudget()` updates (60 unless it overrides it)
  fails, so a stuck step ends the run instead of hanging it. The runner knows
  nothing about the editor and is tested on its own
  (`tests/editor/SmokeRunnerTests.cpp`).
- `EditorSmokeScenario.cpp` builds the editor's runner: one `runner->add`
  line per step, in the order they run. That list is the only place the
  order lives.
- Steps get a `SmokeStepContext`: the frame's `ApplicationContext` and
  `EditorSmokeHooks` (`EditorSmokeHooks.hpp`), the part of the session a step
  may use (operators, undo, selection, history, play mode, viewports, the
  active workspace, menus, the module host, and the play targets). The session
  implements the hooks for the frame being run (`EditorSessionSmoke.cpp`);
  steps never reach into `EditorSession` itself.
- The session creates the runner in `load` only when `ZENITH_EDITOR_SMOKE` is
  set and updates it once per frame, after the workspace switch and before the
  areas are drawn. When the run passes it requests quit. When a step fails it
  reports `Smoke step '<name>' failed: <message>` once (to the log, which the
  console shows, and to diagnostics) and requests an invalid scene, so the
  editor exits with an error.

The `generated preview` step (`GeneratedPreviewSmokeStep.cpp`) builds a
transient entity in front of the active viewport and checks that the scene
document leaves it out, that the viewport keeps projecting a label from a
persistent debug draw channel for frames after it was recorded, and that
hiding the channel removes the label.

## Adding a step

1. Write the step in its own file under `src/editor/smoke/` (or beside the
   feature it covers): a class deriving from `SmokeStep` in an anonymous
   namespace, and a `make...SmokeStep()` factory declared in
   `EditorSmokeSteps.hpp`. Give it a short unique `name()`.
2. Switch on the step frame for multi-frame checks: act on one frame, check
   the result on a later one (anything drawn or rendered shows up a frame
   later). Return `Done` on the last frame; never wait on wall-clock time.
3. Leave the editor as you found it: undo your edits (take a
   `history().marker()` first and `undoSince` it), close areas you opened, and
   restore view state. Later steps assume Edit mode and the startup scene.
4. Add one `runner->add(make...SmokeStep());` line to
   `EditorSmokeScenario.cpp` where its preconditions hold, and the file to
   `src/editor/CMakeLists.txt`.
5. Override `frameBudget()` if the step legitimately needs more than 60
   frames (waiting for a GPU pick, a child process, or a load), and keep the
   sum of all budgets below the `ZENITH_SMOKE_FRAMES` cap in
   `tests/editor/CMakeLists.txt`.
6. If the step needs something of the session the hooks do not offer, add a
   narrow query or action to `EditorSmokeHooks` rather than exposing the
   session.
