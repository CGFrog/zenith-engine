# Prefab windows

With **Open prefabs in their own windows** on (Preferences > Interface,
`ui.prefab_windows`, off by default), Edit Prefab opens the prefab in a
floating window of its own instead of swapping it in over the level, as
Unreal's asset editors do. The level stays open, visible, editable, and
playable underneath, and several prefabs can be open at once, one window
each. With the preference off, prefab mode (the document stack,
[Scene editing](scene-editing.md#prefab-mode-the-document-stack)) is
unchanged.

The window hosts its own dock space with the prefab's **Hierarchy**,
**Inspector**, and a **Viewport** slot, plus Save and Close. The viewport slot
is a placeholder for now: the renderer draws one scene per frame
(`Renderer::drawFrame` takes a single `Scene`, and the mesh pass caches by
scene revision), so showing a second scene live needs per-view scene binding
in the engine. Until then the preference stays off by default.

## Documents

`application/PrefabWindowSet` owns one `PrefabWindowDocument` per open
prefab: the prefab instantiated into a `Scene` of its own
(`openPrefabDocument`), an `EditorDocument` copied from the level's options
(asset root, database, and the game module's component types), its own
`CommandHistory` and `Selection`, the `PrefabEditTarget` (reference, file,
display name, root), and an `AssetLeaseSet` the session renews every frame,
because only the engine's own scene is leased by the application. Documents
are held by address, never moved, and the set never opens the same prefab
twice: opening an open prefab brings its window forward.

A `DocumentBinding` (`application/DocumentBinding.hpp`) names one open
document's scene, document state, history, and selection: the level's are the
session's own members, a window's are its document's.

## Which document an action edits

- **Panels in a window** get an `EditorContext` built from the window's
  document: `app` is the session's `ApplicationContext` with the window's
  scene, so panels that read `context.app.scene` show the prefab, and
  `actions` is `EditorSession::PrefabWindowActions`, which runs operators and
  region keys on the window's document with the session's own context.
- **The main menu, global keys, F3, and popup menus** (Add Component, Delete,
  Rename) act on the document whose window last had keyboard focus: a prefab
  window, until a level area is focused again. Ctrl S, Ctrl Z, and Ctrl Y
  therefore follow focus.
- **Level areas** always act on the level.
- A prefab document is never Play's runtime scene: its `OperatorContext` has
  mode Edit, so it records history and stays editable while the level plays.
  Operators that mean the session's state (Play, New, Open, Save As) read the
  session's play state, not the context's mode.
- Modal G/R/S transforms need a viewport showing the document
  (`EditorObjectModeCommands::viewportsShowDocument`), so they are unavailable
  in prefab windows for now. Adjust Last Operation re-runs on the level only,
  and the eyedropper and watched fields are not offered in windows, since
  both resolve their targets in the level.

## Saving, refreshing, closing

- **Save** writes the prefab (`saveOpenPrefab`, shared with prefab mode):
  the ids it gives entities added since opening are recorded as a step
  "Save <name>" on the window's history. The session then refreshes the
  prefab's instances in the level (its edit scene during Play; the running
  scene keeps its copies until Stop) and in every other window, each as a
  scene-only step "Refresh <name> instances" on that document's own history,
  so Undo there puts the old instances back. The window editing the prefab
  itself is never refreshed.
- **Outside changes** to a prefab file refresh its instances in the windows
  too; a window editing that very prefab only warns, and its Save writes over
  the change.
- **Close** (the window's X, its Close button, or `prefab.exit_edit` on the
  focused window) asks Save, Discard, or Cancel when the window has unsaved
  changes. Windows close between frames, never while their panels draw.
- **New and Open** replace only the level; windows stay open. **Quit** and
  the **Project Hub** ask about each window with unsaved changes first.
- **Moves and removals** follow the prefab file and the window scene's asset
  references; a removed prefab keeps its window with a warning.
- **Module reloads** wait until every window is closed (status bar: "close
  the prefab windows to apply"), because window scenes hold module component
  storage; the session closes every window before the module unloads.
- **Autosave** writes each window with unsaved changes to
  `<settings>/recovery-prefab-windows/<n>.prefab-edit` with its note
  (`document/PrefabWindowRecovery`), replacing the whole set each time, so a
  saved or closed window leaves nothing behind; the crash save does the same.
  Recover opens them again.

## Known limits

- No live 3D view in the window yet (see above).
- A prefab written by `prefab.apply` or `prefab.revert` in one document
  refreshes the instances in that document only, as in prefab mode; other
  open documents catch up when the prefab is saved from a window.
- Undo of a refresh step restores the instances in that document only, not
  the prefab file.

## Folder contents

`src/editor/application/`: `PrefabWindowSet` (the documents),
`EditorPrefabWindows.cpp` (the session's windows, their panels, the close
prompt, and recovery), `PrefabEditTargets` (save and file-move helpers shared
with prefab mode), and `DocumentBinding.hpp`.
`src/editor/document/PrefabWindowRecovery`: the windows' autosaves.
Tests: `tests/editor/PrefabWindowTests.cpp`.
