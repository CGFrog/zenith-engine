# Gameplay roadmap

This roadmap made a typical first-person game programmable end to end from
the editor, as a hot-reloaded C++ game module. It was approved 2026-10-04 and
every phase (A to E) is done; Phase E laid the foundations for a first-person
roguelike, built as a separate project from the FPS template. The phases,
their results, the status table, and the dated decisions are archived in
[Gameplay roadmap 2026](history/gameplay-roadmap-2026.md). This file keeps
the standing decisions, the follow-ups that are still open, and the
conventions for new items.

## How to use this document

- **Before starting.** Pick an item under [Open follow-ups](#open-follow-ups)
  and read the subsystem doc it names (`docs/engine/*.md`, `docs/editor/*.md`).
- **While working.** Follow `AGENTS.md`, the nearest nested `AGENTS.md`, and the
  [Conventions for every item](#conventions-for-every-item) below.
- **When finishing.** Remove the follow-up from this file (name the commit in
  the commit message) and move feature details into the subsystem doc. Note
  any new follow-up under [Open follow-ups](#open-follow-ups).
- **When the plan changes.** Record the decision under [Decisions](#decisions)
  with a date, so later sessions do not reopen it.

## Decisions

The dated decisions of Phases A to E, with their reasons, are in
[Gameplay roadmap 2026](history/gameplay-roadmap-2026.md#decisions). These
still govern new work:

- **2026-10-04: C++ Behaviours on top of the ECS.** Per-entity logic is a C++
  `Behaviour`; plain systems remain the tool for bulk logic. Python and other
  scripting languages were rejected for performance and are not to be
  re-proposed.
- **2026-10-04: engine owns mechanisms, games own rules.** The character
  controller, physics queries, events, timers, effects, save slots and UI
  widgets are reusable engine mechanisms. Weapons, damage, HUD contents and
  game flow stay in game modules and templates.
- **2026-10-04: roguelike scope.** The roguelike's perception model and
  mutable world, and the engine work they need (climb, swim, fly and
  wall-walk movement modes, stimulus events, a CPU light-level query,
  through-walls overlays, water and buoyancy, breakable constraints, and tiled
  navmesh rebuilds), are deferred. A run is saved as its seed, floor and run
  state, and floors are spawned into the main scene.

## Open follow-ups

Follow-ups recorded under the finished items that are still open. The item
that raised each is in brackets; "(unverified)" marks entries not re-checked
against the code when the roadmap was archived.

### Gameplay services and modules

- `GameServices` has no run-save shorthand. A game calls
  `saveGames()->saveData` with `randomStreams().snapshot()` itself; add a
  helper if every game repeats it. [E2]
- The code generators' registration file is always `src/GameModule.cpp`, and
  new files always go in `src/`; there is no setting for a project laid out
  differently. [C2] (unverified)
- No test draws a game view's debug labels (that needs a rendered frame), and
  the watch list is not saved with the session's other settings. [C4]
  (unverified)
- The FPS template's pause menu has no Quit, because the same module runs in
  the editor, where `requestQuit` would close the editor; the template's
  settings are not saved between runs. `ProjectApplication` itself has no pause
  or settings screen. [B6, C6] (unverified)

### Scene, prefabs, and reflection

- About 1 ms of a recorded edit, and of a prefab spawn, still grows with the
  scene; the likely cause, not yet profiled, is that
  `SceneDocument::applyChanges` validates every collection reference of the
  resulting scene. Some prefab, collection and module operators still capture
  the whole scene. [D4] (unverified)
- No lists of plain values (strings, numbers) in reflection yet; a list of
  one-field structs works meanwhile. The eyedropper cannot pick an entity
  reference inside a list. [E3]
- A save baseline written in an older document version diffs against every
  entity, so such saves are larger until the level is re-saved; undo
  snapshots leave runtime state out, so an undone Play-mode edit resets
  runtime fields to their defaults. [B5] (unverified)
- Animator states address clips by name; renamed clips leave animators inert
  without a diagnostic. Both clips of a cross-fade share the target state's
  speed and looping. [B3] (unverified)

### Physics, navigation, and input

- Characters do not push each other (Jolt's character-vs-character collision
  is not set up), and a character leaving a moving platform loses the
  platform's velocity. [B1] (unverified)
- Removing a physics layer from the middle of the table is not offered; it
  would need remapping every body's index. [C5] (unverified)
- Navigation meshes are baked at runtime; save baked meshes (with the scene or
  as an asset) and use tiles for large worlds. [B7] (unverified)
- Crowd agents accelerate almost at once (100 m/s²), leaving smoothing to
  character controllers; a transform-driven agent that should ease in needs a
  per-agent acceleration. When a crowd is full, the first admitted agent
  follows the ECS view's order, not creation order; a game that caps its crowd
  may want a priority. [E5] (unverified)
- A player's remapped `InputProfile` is not saved per game and applied at
  start, and key rebinding is not on the settings screen (the input layer has
  no "any key pressed" query). [A4, B6, C5] (unverified; the demo's
  `GameSettings` now stores profile names)

### Rendering, UI, audio, and effects

- The editor's game-view overlay draws text in the ImGui font and images as
  their tint; a per-view GameUi pass in the renderer would remove the
  difference. The System Profiler counts Ui system time in the following
  frame, because the profile frame ends at the start of `drawUi`. [A2]
  (unverified)
- Pooled effect instances stay visible while parked (`Effects.hpp`), so
  pooling suits particle-only effects; pools are unbounded and live as long as
  the gameplay session. [B4]
- Audio occlusion is all or nothing per ray and ignores project layers other
  than Static; partial occlusion (several rays, materials) is later work. [B8]
  (unverified)
- The inspector edits `clip_asset` as text; dropping a clip from the Asset
  Browser onto it would be friendlier. Emitters could preview in Edit mode;
  they load only while gameplay runs. [E6] (unverified)

### Build and measurement

- The CMake toolchains do not set `-ffp-contract=off`; add it when a GCC or
  Clang build ships, so seeded random streams stay bit-identical. [E1]
- The floor benchmark uses box colliders. Runtime meshes
  (`AssetManager::createMesh`) now exist, so triangle-mesh colliders can be
  measured; do that when the game's room kit exists. [E4] (unverified)

### Watch items

- `zenith_file_watcher_polling` hung twice under the parallel CPU suite; the
  test now retries and has a 60 s timeout. Reopen if it comes back.
- `zenith_showcase_capture_fsr1_upscale` failed once under parallel Vulkan
  load and passed alone; likely a flake.
- Hot reload during Play is out of scope; reloads stay queued until Stop.

## Not in Phase E

Each of these waits until the roguelike's first playable slice shows it is
needed:
- inventory slot grids, drag and drop, tooltips;
- world-space UI (damage numbers, health bars, prompts);
- the game font in the editor's game view;
- `ButtonBehavior::Toggle`, an any-key query and full rebinding;
- animation blend trees and layers;
- particle flipbooks and trails;
- hinge constraints and an angular velocity API;
- a deterministic replay harness.

## Conventions for every item

- Configure worktrees with `ZENITH_SHARED_DEPS_DIR` and `cmake --preset
  worktree` (see `AGENTS.md`).
- Run `tools/check.ps1` before handing off, and `tools/check.ps1 -Full` at the
  end of a batch.
- New editor features are operators and registry-driven (see
  [Operators and keymaps](editor/operators.md)).
- Register new tests above the per-build `test-temp` fixture block in
  `tests/*/CMakeLists.txt`.
- New editor smoke steps must use a free frame window (see
  [Editor smoke](editor/smoke.md)).
- Graphics or UI changes also need a bounded Vulkan smoke run
  (`ZENITH_SMOKE_FRAMES`).
- Changing a serialized field's key or meaning needs a schema version and a
  migration.
