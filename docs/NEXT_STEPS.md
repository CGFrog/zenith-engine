# Zenith next steps

This backlog is ordered to produce a usable level-design workflow without
coupling editor policy to reusable engine systems. Complete each milestone's
exit criteria before expanding its scope.

## Milestone 1: editor foundation

- [x] Add an `src/editor/` application target that depends on `zenith_engine`.
- [x] Keep editor panels, selection, projects, and commands out of
      `src/engine/` and `src/game/`.
- [x] Define `EditorSession` as the owner of editor-only state.
- [x] Add an ImGui docking shell with View, Edit, File, and Play menus.
- [x] Add persistent panel visibility and layout settings.
- [x] Add a status bar for diagnostics and the existing render statistics.
- [x] Document editor-to-engine dependency direction in `ARCHITECTURE.md`.

Exit criteria:

- The game and editor are separate executables using the same engine library.
- Closing or reopening panels does not affect the runtime scene.
- The existing game continues to build and run without editor dependencies.

## Milestone 2: scene document and persistence

- [x] Introduce stable scene entity IDs independent of EnTT entity values.
- [x] Define a versioned, human-readable scene document format.
- [x] Serialize names, hierarchy, local transforms, and component data.
- [x] Store asset references as project-relative paths or stable asset IDs,
      never transient runtime handles.
- [x] Validate duplicate IDs, invalid parents, hierarchy cycles, missing
      assets, and unsupported document versions.
- [x] Return descriptive load/save errors without partially replacing the
      active scene.
- [x] Implement New, Open, Save, Save As, and dirty-document tracking.
- [x] Use an atomic temporary-file-and-rename save path.
- [x] Add round-trip, malformed-input, missing-asset, and version tests.

Exit criteria:

- A scene can be saved, restarted, and loaded without observable changes.
- Failed loads preserve the currently open scene.
- Persistence tests are CPU-only and deterministic.

## Milestone 3: hierarchy and inspector

- [x] Add a hierarchy panel with selection, rename, create, duplicate, delete,
      and reparent operations.
- [x] Prevent cycles and preserve world transforms when reparenting by choice.
- [x] Add multi-selection without changing the core scene API unnecessarily.
- [x] Add an inspector panel for local transform and existing components.
- [x] Make component editors small, independently testable adapters.
- [x] Support adding and removing optional components.
- [x] Display invalid or missing asset references clearly.
- [x] Add tests for hierarchy mutation and component lifecycle behavior.

Exit criteria:

- Designers can construct and organize a scene without editing source files.
- All hierarchy operations preserve scene invariants.

## Milestone 4: command history

- [x] Define a narrow editor command contract with execute and undo behavior.
- [x] Route document mutations through the command history.
- [x] Implement undo/redo for entity creation, deletion, reparenting, renaming,
      component changes, and property edits.
- [x] Coalesce continuous transform or numeric-field drags into one command.
- [x] Define history behavior across Save, Open, New, and play-mode changes.
- [x] Add deterministic command and history-limit tests.

Exit criteria:

- Every destructive editor operation in milestones 2 and 3 is reversible.
- Undoing and redoing returns the serialized document to identical content.

## Milestone 5: editor viewport

- [x] Add a renderer-owned offscreen viewport target with explicit resize and
      lifetime handling.
- [x] Present the viewport texture in an ImGui panel without exposing Vulkan
      details to editor code.
- [x] Add an editor camera with orbit, pan, fly, focus, and configurable speed.
- [x] Route viewport input through the existing capture model.
- [x] Add entity picking using a deterministic ID attachment or bounded CPU
      ray test; choose after measuring both approaches.
- [x] Add selection outline or another low-cost selection indicator.
- [x] Integrate translate, rotate, and scale gizmos.
- [x] Keep gizmo operations in local/world modes with snapping controls.
- [x] Treat one gizmo drag as one undoable command.

Exit criteria:

- Clicking visible geometry selects the correct entity.
- Viewport resizing and docking do not leak or recreate unrelated resources.
- Transform editing works from both the inspector and gizmos.

## Milestone 6: project and asset browser

- [x] Define a small project configuration containing asset and scene roots.
- [x] Add an asset browser for models, textures, audio, and scene files.
- [x] Move directory scanning and metadata extraction outside rendering code.
- [x] Add file-change detection with explicit, recoverable reload behavior.
- [x] Support dragging models into the viewport and assets onto compatible
      inspector fields.
- [x] Cache thumbnails asynchronously without blocking frame rendering.
- [x] Report import and reload errors in an editor diagnostics panel.

Exit criteria:

- A model can be found, placed, saved, and restored entirely through the UI.
- Asset refresh work cannot invalidate resources still used by an in-flight
  frame.

## Milestone 7: edit and play modes

- [x] Define explicit Edit, Play, and Paused states in the editor layer.
- [x] Instantiate a separate runtime scene when entering Play.
- [x] Keep physics and gameplay mutations out of the editable document.
- [x] Restore the editor scene and selection when leaving Play.
- [x] Add play, pause, single-step, and stop controls.
- [x] Define which runtime changes, if any, may be applied back deliberately.
- [x] Test repeated mode transitions and restoration after entity deletion.

Exit criteria:

- Stopping play always restores the exact pre-play serialized scene.
- Repeated mode transitions do not leak physics, audio, or GPU resources.

## Milestone 8: renderer follow-up

- [x] Cache render packets and update them only when topology, material, or
      transforms change; benchmark before and after.
- [x] Add dirty transform propagation to avoid full hierarchy recomputation.
- [x] Replace synchronous upload command pools and queue-idle waits with a
      reusable staging/upload context.
- [x] Implement true downsampled bloom before exposing higher bloom presets.
- [x] Keep temporal antialiasing unavailable until motion vectors and automated
      ghosting and stability captures exist; expose only Off and true FXAA.
- [x] Add cascade visualization and tune split blending and bias controls.
- [x] Add texture mip generation and choose anisotropy from device limits.
- [x] Prototype indirect/bindless submission only behind capability checks;
      retain the compatible instanced path and adopt the prototype only if the
      2,000-object benchmark improves materially.
- [x] Add deterministic screenshot comparisons for color-space, shadow, and
      anti-aliasing regressions.

Exit criteria:

- No default-quality change regresses median or p95 CPU/GPU frame time beyond
  the agreed measurement tolerance on the same machine and driver.
- Every quality option names only behavior that is actually implemented.

## Milestone 9: editor hardening

- [x] Add unsaved-change prompts for close, open, and new operations.
- [x] Add autosave/recovery without overwriting the primary document.
- [x] Add copy/paste and duplicate support based on serialized fragments.
- [x] Add keyboard shortcuts with conflict detection and discoverable labels.
- [x] Add structured editor diagnostics and actionable error messages.
- [x] Add a headless editor-document test suite to CI.
- [x] Add bounded Vulkan smoke tests for viewport resize and play transitions.
- [x] Document a minimal level-authoring tutorial.

Exit criteria:

- Common failure paths preserve user work.
- A new contributor can build the editor and author a simple level from the
  repository documentation.

## Recommended first implementation slice

Start with these tasks, in order:

1. Create the separate editor executable and docking shell.
2. Add stable entity IDs and scene serialization tests.
3. Implement hierarchy selection and transform inspection.
4. Add command-based undo/redo for those operations.
5. Add the offscreen viewport, picking, and transform gizmos.
6. Finish with Save/Open and an end-to-end authored-scene test.

Do not begin the asset browser or play mode until this slice can create, edit,
save, reload, and undo changes to a small scene reliably.
