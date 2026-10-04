# Operators and keymaps (`src/editor/operators/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Blender-style operators: every menu item, button, and key runs an `Operator`
(stable id, label, description, `poll`, `execute`, properties described by
reflected `Fields<T>`, and `Undo`/`Register` flags) registered in the
`OperatorRegistry`; game modules register their own (see Game module hosting). The
`OperatorRunner` checks poll, wraps `Undo` operators in one `SceneEditing`
undo step, reports failures to diagnostics, records every invocation in an
`OperatorHistory` (shown by the Info panel), and keeps the last `Register`
operator so the Adjust Last Operation panel can undo it and re-run it with
edited properties. `Keymap` binds key chords, and in the Viewport region mouse
chords (LMB, MMB, RMB, X1, X2 with modifiers), to operator ids per region
(Global, Viewport, Outliner) with a `KeyTrigger` (Press, DoubleTap, Hold,
Release, Click, Drag) and conflict detection per chord and trigger; only the
user's differences from `DefaultKeymap` are saved to `keymap.json` in the
editor settings directory. `KeymapDispatch` turns ImGui input into bindings
to run: `KeymapDispatcher` polls key and mouse presses and follows a press as
a `KeymapGesture` (pure, tested with synthetic frames) for its Hold, Drag,
Release, and Click bindings. The session updates it once per frame and the
viewport asks it, through `EditorSessionActions::dispatchMouseChords` and
`mouseButtonBound`, which operator a mouse chord runs (select, Alt+click
cycle, the Ctrl+RMB drag lasso, the Shift+RMB 3D cursor), so unbound right
and middle presses stay camera navigation. `OperatorSearch` is the F3 popup
and `KeymapEditor` the rebinding window (keys, viewport mouse chords, and a
trigger per binding). `ObjectOperators` holds the document-only object
operators, which run headless in `tests/editor/OperatorTests.cpp`, and
registers `CollectionOperators` (`collection.new`, `delete`, `link`, `unlink`,
`set_flag`, and `object.collection_instance_add`) and `VisibilityOperators`
(`object.hide` H, `object.unhide` Alt+H, `object.isolate` Shift+H). The
viewport hide is editor-only: `EditorSession` renders with
`SceneVisibilityMode::Editor` while editing and `Runtime` in Play mode, and
hidden or unselectable entities (or collections) cannot be picked or selected
with Select All. `ComponentOperators` holds the component value operators (reset, copy, paste, copy
to selected, move), tested in `tests/editor/ComponentOperatorTests.cpp`.
`EyedropperOperators` arms (`ui.eyedropper_entity`), completes
(`ui.eyedropper_pick`, one undo step), and cancels (`ui.eyedropper_cancel`) the
session's `interaction/EntityEyedropper`.
`PrefabOperators` holds `prefab.create_from_selection`, `prefab.instantiate`,
`prefab.apply`, `prefab.revert`, `prefab.unpack`, `prefab.revert_field`, and
`prefab.select_asset` (reveals the prefab in the primary Asset Browser through
`ProjectAssets::requestReveal`), built on `document/PrefabAuthoring` (which
also answers `PrefabAuthoring::overrides(entity)` for override highlighting,
cached by `PrefabOverrideCache`: the prefab is prepared once per write time
(`PrefabSystem::prepareComparison`), the whole instance is compared once, and
each new document revision recompares only the shown entity
(`PrefabSystem::entityOverrides`) until edits settle for 250 ms, so dragging a
value in a 200-entity instance stays cheap; `PrefabDisplayNames` reads display
names for Add > Prefab once per file write time,
and suggests `prefabs/<name>.prefab` when create gets no path); they run
headless in `tests/editor/PrefabTests.cpp`, and the UI flows (field override
marks and Revert to Prefab, the instance header, Add > Prefab, asset drops) in
`tests/editor/PrefabUiTests.cpp`. Each is one undo step for its scene changes,
while the prefab files they write stay as written on undo. The Add menu's
Prefab section lists the asset catalog's `.prefab` files
(`EditorAssetKind::Prefab`), and the Asset Browser drags them with the
`ZENITH_PREFAB_ASSET` payload (`panels/AssetDragPayloads.hpp`).

## Operator families

The editor's own operators are registered family by family from one table,
`editorOperatorFamilies()` in `operators/EditorOperatorFamilies.cpp`. The
session builds one `EditorOperatorServices` (references to the document,
history, menus, viewports, workspaces, play mode, module host, preferences,
package runner, and the like, plus the session's workflows) and runs every
family in table order, reporting a failure as `"<family>: <error>"`. A family
stops at its first rejected operator. Table order is registration order, which
is the F3 search order among equal matches; menus and keymaps name operators
by id only when they run, so nothing else depends on it.
`tests/editor/OperatorFamilyTests.cpp` runs the whole table headless and
checks that every family registers and no id is registered twice.

Operators that start a session workflow (file commands, undo, Play, modal
transforms, the F3 search, workspaces, module reload) reach the session only
through the narrow interfaces in `application/EditorSessionCommands.hpp`
(`EditorDocumentCommands`, `EditorPlayCommands`, `EditorObjectModeCommands`,
`EditorWindowCommands`), which `EditorSession` implements. Those families are
declared in `application/EditorSessionOperators.hpp` and defined beside the
session code they drive. Non-operator registrations (`registerAreaTypes`,
`registerDefaultMenus`) stay explicit calls in the session.

### Adding an operator family

1. Write the family's register function next to its operators, taking only
   the services it needs (its own small services struct, as
   `PlayOperatorServices` or `PackageOperatorServices` do).
2. In the same `.cpp`, add the adapter
   `OperatorRegistrationResult registerXOperatorFamily(OperatorRegistry&, const EditorOperatorServices&)`
   that builds those services from `EditorOperatorServices`, and declare it in
   the family's header.
3. Add one line to the table in `EditorOperatorFamilies.cpp`:
   `OperatorFamily{"x", registerXOperatorFamily},`.

If the family needs something the session owns that `EditorOperatorServices`
does not hold yet, add a reference to it there and in
`EditorSession::operatorServices()` (and the test's services); if it needs a
session workflow, add a method to the fitting `EditorSessionCommands`
interface rather than passing the session.

## Folder contents

`src/editor/operators/`: Operator registry and runner (poll, execute, undo,
Adjust Last Operation; operators run on the runtime scene without undo in Play
and Simulate), headless object, component, collection, visibility (hide),
outliner, and prefab operators, the headless play operators (time scale,
Simulate systems, profiler reset, Apply to Edit Scene), the preferences and
Build and Package operators, the per-region keymap (key and viewport mouse
chords, Press/DoubleTap/Hold/Release/Click/Drag triggers) with `keymap.json`
overrides and its input dispatch (`KeymapDispatch`), F3 search, and the keymap
editor.
