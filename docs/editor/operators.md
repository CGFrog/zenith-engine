# Operators and keymaps (`src/editor/operators/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Blender-style operators: every menu item, button, and key runs an `Operator`
(stable id, label, description, `poll`, `execute`, properties described by
reflected `Fields<T>`, or by run-time descriptors through
`OperatorProperties::fromDescriptors` as a game module's are, and
`Undo`/`Register` flags) registered in the
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
registers `CollectionOperators` (`collection.new`, `delete`, `rename`, `link`,
`unlink`, `set_flag`, and `object.collection_instance_add`),
`OutlinerOperators` (`object.set_visibility`, `outliner.parent_drop`,
`outliner.reorder {entity, parent, index, keep_transform}`, which moves one
object to a position among a parent's children, and the non-undoable
`outliner.select_matching {filter}`, which selects every object an outliner
search matches), and `VisibilityOperators`
(`object.hide` H, `object.unhide` Alt+H, `object.isolate` Shift+H).
`collection.rename {collection, name}` trims the name and refuses an empty
one; names need not be unique. The
viewport hide is editor-only: `EditorSession` renders with
`SceneVisibilityMode::Editor` while editing and `Runtime` in Play mode, and
hidden or unselectable entities (or collections) cannot be picked or selected
with Select All. The Add menu's operators (`object.add_*`, including
`object.add_camera`, primary only when the scene has no primary camera, and
`object.add_empty_with`, `object.add_collider`, and `prefab.add_character`) poll Edit mode, as the menu does, so a key, the menu,
and F3 follow the same Play rules; `object.create_entity`, duplicate, and
delete still edit the running scene live.

`PlacementOperators` (the `placement` family) holds `object.place_model
{asset, parent, location, rotation, normal, align_to_normal}` (Undo,
Register; Edit mode only), which every model placement runs
(`EditorContext::placeModel`, viewport drops with the surface under the
mouse), `object.array {count, offset, rotation_deg, scale_step,
relative_to_bounds}`, and `object.scatter {count, radius, seed,
align_to_surface, random_yaw, scale_jitter}`. `prefab.instantiate` takes the
same `rotation`, `normal`, and `align_to_normal`; `placementRotation` turns
+Y onto the normal, then applies the Euler rotation (degrees). Array and
scatter copy the selection's transform roots into one scene fragment,
replicate it once per copy (`SceneFragment::replicate`), and paste it once;
each copy joins its original's parent
(`EntityCommands::pasteBesideSources`), the copies are selected, and both are
one undo step that Adjust Last Operation re-runs. Array steps each copy from
the last (turn and scale about the copies' median, then the offset turned
with them, so a turning array curves; `relative_to_bounds` measures the
offset in selection bounds). Scatter draws every copy's random values from a
local `RandomStream(seed)` whatever the options, so the same seed gives the
same copies, and drops each onto the surface below a random point of the disc
(downward rays that skip the originals, `surfaceHitAlongRay`).
`tests/editor/PlacementTests.cpp` covers them headless.

`ModelOperators` (the `model` family) holds `object.model_replace {entity,
asset}` (see the [inspector](inspector.md)) and `object.mesh_assign
{entities, asset, mesh, update_collider}` (Undo, Register; Edit mode only),
which gives every listed entity (the selection when none are listed) the
glTF mesh `mesh` of the model `asset` as one undo step, adding a Mesh
Renderer where there is none. A model node that gets another mesh leaves its
model instance: its Model Node goes (the model's animation no longer moves
it, and a later model replace treats it as a user object), with an Info
diagnostic. A Skin stays only when the skin's model draws the new mesh with
that skin on some node; otherwise it is removed, so a static mesh is never
drawn with a stale joint palette. Morph Weights stay when the new mesh has as
many targets, are reset to the model's defaults when the count differs, and
go when it has none. A skeleton joint is refused, since removing its Model
Node would break the skin following it. `update_collider` (off by default,
since collision shapes are often simpler than the drawn mesh) also points a
triangle-mesh collider on the entity that used the previous mesh at the new
one. A scene holding a runtime mesh (`AssetManager::createMesh`) cannot be
saved, so the runner cannot record the step and the operator is refused.
`tests/editor/MeshAssignTests.cpp` covers it with the inspector row.

`SelectOperators` (the `select` family) holds `object.select_hierarchy
{direction, extend}` (`]` children, `[` parent, Shift extends) and
`object.select_grouped {type, extend}` (Shift+G opens its menu, also in Edit >
Select Grouped): children, immediate children, prefab, model, mesh, tag,
collection, or components (every component type the active object has).
They record no undo step, pick only `SceneVisibility::selectable` entities,
and cancel without changing the selection when they find nothing;
`tests/editor/SelectionToolsTests.cpp` covers them. `ComponentOperators` holds the component value operators (reset, copy, paste, copy
to selected, move), tested in `tests/editor/ComponentOperatorTests.cpp`.
`EyedropperOperators` arms (`ui.eyedropper_entity`), completes
(`ui.eyedropper_pick`, one undo step), and cancels (`ui.eyedropper_cancel`) the
session's `interaction/EntityEyedropper`.
`PrefabOperators` holds `prefab.create_from_selection`, `prefab.instantiate`,
`prefab.add_character` (Add > Physics > Character: instantiates the project's
`prefabs/Character.prefab`, writing it from a new first-person player the first
time, as one undo step with the file),
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
`tests/editor/PrefabUiTests.cpp`. Each is one undo step for its scene changes.
`prefab.apply` and `prefab.create_from_selection` also write their prefab
file, and that write is part of the same step: the operator returns an
`OperatorResult::undoStep` factory, and the runner records its scene diff
through it as one `PrefabWriteCommand` instead of a plain `EntityDiffCommand`
(an `Undo` operator still never commits history itself). Undoing Apply puts
back the file's previous content and the other instances; undoing Create
removes the new file and unlinks the entity; redo writes the file again.
Undo and redo never overwrite a file changed outside the editor since the
step: they fail with "<file> changed on disk since this step; undo skipped"
and change nothing (see [Scene editing](scene-editing.md#prefab-file-writes)).
`tests/editor/PrefabWriteTests.cpp` covers the file side. The Add menu's
Prefab section lists the asset catalog's `.prefab` files
(`EditorAssetKind::Prefab`), and the Asset Browser drags them with the
`ZENITH_PREFAB_ASSET` payload (`panels/AssetDragPayloads.hpp`).
`prefab.edit {asset}` opens a prefab as a document of its own (prefab mode;
the asset is project-relative or absolute inside the asset root, and empty
means the active entity's nearest instance's prefab), and `prefab.exit_edit`
returns to the document under it, asking about unsaved changes. Both reach
the session through `EditorPrefabModeCommands` (`PrefabOperatorServices::
prefabMode`; without it they never poll) and run only in Edit mode with no
file dialog or modal tool; they record nothing themselves. In prefab mode
`file.save` writes the prefab, `file.save_as`, `play.start`, and
`play.simulate` are disabled, and going back after saving records one
"Edit <name>" step on the document below (see
[Scene editing](scene-editing.md#prefab-mode-the-document-stack)).
`prefab.instantiate` also reports the files it read
(`PrefabWriteHooks::fileRead`), which seeds the session's write ledger.

## Declaring what an operator changes

An `Undo` operator's undo step is the difference between the scene before
and after it runs. Without more information the runner must serialize the
whole scene twice for that (and once more for Adjust Last Operation's check),
which costs hundreds of milliseconds in a 5,000-entity scene and fails when
anything in the scene cannot be serialized (a runtime mesh). An operator that
knows what it touches declares it in `Operator::scope`, an
`OperatorScopeFunction` the runner calls with the context and the properties
it is about to run with, before `execute`:

| Scope | Use for | Examples |
| --- | --- | --- |
| `newEntitiesScope()` (`OperatorScope::newEntities()`) | creating entities at the top level only | `object.add_*`, `edit.paste`, `object.collection_instance_add` |
| `selectionScope()` | changing the selected entities, or adding copies beside them (a copy joins its original's parent, which is captured as the parent of a listed entity) | clear transform, component add/remove/reset/paste, `object.hide`, `object.duplicate`, `object.array`, `object.scatter`, `collection.link`/`unlink` |
| `selectionScope(CaptureReach::SubtreesAndAncestors)` | destroying, unlinking, or moving subtrees (a prefab entity's removal is recorded on its instance root) | `object.delete` |
| `activeEntityScope()` | the active entity, or adding a child or sibling to it | `object.rename`, `object.create_entity` |
| `collectionsScope()`, or `CaptureCollections::Include` with entities | changing collection definitions | `collection.set_flag`, `collection.rename`, `collection.new` |
| a lambda returning `OperatorScope::of(ids, reach, collections)` | entities named by properties | `object.set_visibility`, `outliner.parent_drop`/`reorder` (moved subtrees, ancestors, and the new parent), `prefab.instantiate`/`object.place_model` (the `parent` property), `object.mesh_assign`, `prefab.unpack`, `object.unhide`, `ui.eyedropper_pick` |

Entities the operator creates are always found (ids from
`SceneEditing::EntitySnapshot::firstNewId` up) and recorded, and each listed
entity's parent is captured too. Everything else is the operator's promise:
a change to an entity outside its scope is not undone, and destroying one is
reported as an error. An operator whose reach is unclear leaves `scope` empty
and keeps the whole-scene capture (`prefab.create_from_selection`,
`prefab.apply`, `prefab.revert`, `prefab.revert_field`, `collection.delete`,
`object.isolate`, `object.model_replace`, and game module operators).
`tests/editor/ScopedCaptureTests.cpp` runs every scoped operator with and
without its scope and requires the same scene after the run, undo, and redo;
add a case there when you declare a scope. In Play and Simulate nothing is
recorded, so the runner captures only the scope (to tell the renderer and
physics what changed), and Adjust Last Operation compares only the scope's
entities with what the operator left (`SceneEditing::unchangedSince`).

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

The `placement` and `select` families are described above. The `navigation` family (`NavigationOperators.*`) bakes and clears the
preview navigation mesh the session owns (see
[Navigation](../engine/navigation.md)). The `build` family
(`BuildOperators.*`) starts and cancels the session's build, and the `code`
family (`CodeOperators.*`) runs the code generators (see
[Build and IDE workflow](build-ide.md)).

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
and Simulate), headless object, placement (place model, array, scatter),
selection, component, collection, visibility (hide), outliner, and prefab
operators, the headless play operators (time scale,
Simulate systems, profiler reset, Apply to Edit Scene), the preferences and
Build and Package operators, the per-region keymap (key and viewport mouse
chords, Press/DoubleTap/Hold/Release/Click/Drag triggers) with `keymap.json`
overrides and its input dispatch (`KeymapDispatch`), F3 search, and the keymap
editor.
