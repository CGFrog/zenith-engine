# Zenith next steps

This backlog grows the completed level-design workflow into a shippable
general-purpose 3D game pipeline without coupling editor or game policy to
reusable engine systems. Complete each milestone's exit criteria before
expanding its scope.

Milestones 1 to 21 and 23 to 25 are complete. They, and the completed
items of Milestone 22, are archived in
[Completed milestones](history/milestones.md). This file keeps only the open
items with their exit criteria.

## Milestone 22: renderer validation and hardening

The renderer modernization (bindless materials, GPU-driven culling, BC
textures, packed vertex streams, LODs, IBL, clustered lighting, depth prepass,
GTAO, Hi-Z SSR, TAAU, bloom, AgX, and auto exposure) has run on one GPU, without
Vulkan validation, on two small scenes. Harden it before adding features.

- [ ] Test on at least one AMD or Intel GPU, including the update-after-bind
      bindless path and the RGBA8 fallback for devices without BC support.
- [ ] Push the renderer work and confirm the Windows, Linux, and macOS CI jobs.

Completed items are in [Completed milestones](history/milestones.md#milestone-22-renderer-validation-and-hardening-completed-items).

Exit criteria:

- The validation matrix runs with zero validation or synchronization errors.
- Showcase captures are stable across runs on the reference machine, and every
  quality option still produces its named behavior or an explicit fallback.

## Test suite follow-ups

The suite runs in parallel, captures one reference view per CTest, and has
direct tests for reflection, `InputManager`, and the headless `EditorSession`.
Remaining coverage gaps, highest value first:

- [ ] Additive-scene tests: broken references and additive load/unload
      ownership (today only sections of `ProductionMilestonesTests.cpp`).
      Prefabs have `tests/editor/PrefabTests.cpp`: registry round trip with
      module and unknown components, version 1 migration, link persistence by
      GUID, field-level overrides, nested prefabs and cycles, and every
      `prefab.*` operator with undo and redo.
- [ ] `JobSystem` tests: ordering, dependencies, exceptions, and shutdown while
      busy.
- [ ] `ThumbnailCache` and `HierarchyPanel` tests: downsampling,
      request/find/invalidate, worker shutdown, filtering, and reparent drops.
- [ ] Cook optimized meshes and LOD chains into the derived-asset cache, as
      textures are, so imports skip meshoptimizer at load time.

The finished follow-up (recording editor edits without serializing the whole
scene) is in [Completed milestones](history/milestones.md#test-suite-follow-ups-completed).

## Recommended next implementation slice

- Close Milestone 22: run the validation matrix on an AMD or Intel GPU
  (update-after-bind bindless and the RGBA8 fallback; `ZENITH_FORCE_UPDATE_AFTER_BIND`
  and `ZENITH_DISABLE_BLOCK_COMPRESSION` exercise those paths on the reference
  machine), and confirm the Windows, Linux, and macOS CI jobs.
- Work through the test suite follow-ups above, highest value first.
- Gameplay follow-ups are tracked in the
  [gameplay roadmap](GAMEPLAY_ROADMAP.md#open-follow-ups); the
  [roguelike readiness review](ROGUELIKE_READINESS_REVIEW.md) lists the editor
  and engine work still open for the roguelike (its critical items are done).

The [gameplay API](engine/gameplay-api.md) is a contract the engine's headers
are checked against; modules can still include host-only headers. Narrowing the
include directories `Zenith::Engine` exports to modules is later work.

Networking, multiplayer prediction, editor collaboration, advanced vehicles
and soft-body physics, and a stable native binary-plugin ABI remain later work
that should begin only when a concrete game supplies requirements and testable
budgets.
