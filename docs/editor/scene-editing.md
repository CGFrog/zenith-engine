# Scene editing, history, and live inspection

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`SceneEditing` records only the entities an edit added, removed, or changed,
as an `EntityDiffCommand`, so history memory and undo/redo time scale with the
edit rather than the scene. `CommandHistory::storedBytes` reports what the
history keeps.

There are two ways to capture an edit:
- **Only the entities the edit changes** (`captureEntities`, then `commit`
  with the `EntitySnapshot`). The inspector, the transform widget and gizmo,
  modal G/R/S, `EntityCommands`, and every operator that declares a scope
  (`Operator::scope`, see [Operators](operators.md#declaring-what-an-operator-changes))
  use it. By id, it takes a `CaptureReach` (the entities, their subtrees, or
  their subtrees and ancestors, which deleting or moving a prefab instance's
  entity needs for the removal record on the instance root) and, with
  `CaptureCollections::Include`, the collection definitions
  (`SceneDocument::serializeCollections`). Each listed entity's parent is
  always captured too, since a parent's document form holds its children's
  order. Entities the edit creates are found by id and recorded too.
  Destroying an entity the snapshot did not list (counted by entity, so a
  listed entity's transient children count as listed) is reported, because
  undo could not bring it back. `changesSince` returns the changes without
  recording them and an after snapshot, which `unchangedSince` compares
  later (Adjust Last Operation's check); `record` records changes.
- **The whole scene** (`capture`, then `commit` or `perform`). Use it only
  when the edit's reach is unknown: operators without a scope, refreshing
  prefab instances, prefab mode's document switches. It also fails when
  anything in the scene cannot be serialized (a runtime mesh), while a
  scoped capture fails only when the captured entities cannot be.

In a 5,000-entity scene (`zenith_scene_editing_benchmark_run`, Release, one
machine; whole-scene numbers vary with load), recording an inspector edit
costs about 3 ms against about 250 ms for the whole scene, and operators run
through the runner cost (before, with whole-scene capture and paste, against
after): rename 203 ms to 2.2 ms, duplicate a 10-entity group 585 ms to
3.3 ms, array of 10 copies of it 670 ms to 9 ms, delete it 226 ms to 2.9 ms;
`SceneFragment` copy of the group 97 ms to 0.23 ms and paste 158 ms to
0.18 ms. Opening or creating a document still replaces the whole scene and
clears the history. `CommandHistory::marker` and `stepsSince` count the undo
steps recorded after a point of the timeline (a difference of `size()` is no
count once new steps discard redo steps); the smoke steps undo exactly their
own edits with `EditorSession::undoSince`.

Live inspection: in Play and Simulate the session builds `SceneEditing` over
the runtime scene with `EditorPlayMode::editScene()` (the parked edit scene)
attached, so `SceneEditing::runtime()` is true and every panel, the gizmo, the
Item tab, and operators edit the running scene through the same calls (modal
G/R/S still start only in Edit mode, but commit through the same editing)
while `commit`, `perform`, `undo`, and `redo` record nothing (no
runtime undo; Stop discards it). The Inspector and Hierarchy stay enabled
(no panel overrides `EditorPanel::editModeOnly` now), show a runtime-only
notice, and draw running values every frame. `OperatorRunner` runs operators
on the runtime scene without an undo step or Adjust Last Operation and notes
an `Undo` operator's change in Info as lost on Stop (it captures only the
operator's scope, just to mark the changed entities for the renderer and
physics; only an operator without a scope still serializes the whole
scene); object and component
operators allow it, while document-only ones (save, prefabs, collections,
visibility, bakes) keep polling Edit mode, and the runner's refusal then says
they work only in Edit mode. `play.apply_to_edit {entity, component}`
(`operators/PlayOperators`) diffs the runtime entity against the edit scene
(`document/RuntimeApply`: one component, or the whole entity's name,
transform, and components, creating a runtime-only entity) and records the
difference with `SceneEditing::applyToEditScene` as an `EntityDiffCommand` on
the edit history that patches the parked edit scene immediately: Stop restores
the patched scene, so undo and redo after Stop act on it like any edit, and the
runtime scene is never changed. Equal values record nothing.

Transient entities (generated previews, `Scene::isTransient`, see
[Scene](../engine/scene.md#transient-entities)) stay out of the history:
captures serialize the scene without them, so an edit that only builds or
changes a preview records nothing, a mixed edit records only its saved
entities, and undo and redo leave a preview in place (removing a saved entity
removes its transient children, as deleting it did). Copy skips them, paste
leaves them in place and pastes at the top level under a transient parent
(copies under it would be transient too), saves and autosaves leave them out, and Play
enters without them (the parked edit scene keeps them for Stop). The
Hierarchy dims their rows with a "generated, not saved" tooltip.
`zenith_transient_editing` covers it.

Copies keep their place. `SceneFragment::copy` writes each copied root
without a parent but with its `world_transform` beside its local transform;
`paste` at the top level puts a root on that world transform (pasting a
copied child no longer moves it by its parent's offset), while under a parent
it keeps the local transform. Duplicating (`object.duplicate`,
`EntityCommands::pasteBesideSources`) pastes at the top level and then gives
each copy its original's parent and local transform, so duplicating several
children of different parents keeps every copy beside its original.
`SceneFragment::replicate` turns one fragment into N copies with their own
root transforms (entity references inside each copy follow to that copy), so
`object.array` and `object.scatter` paste once, however many copies they
make.

Copies are named apart from their originals (`document/CopyNaming`):
`SceneFragment::paste` renames each pasted root whose name something in the
scene already has. `CopyNamer` splits a name into a base and a trailing
`_<digits>` counter and gives the copy `<base>_<n>`, where n is one more than
the largest counter any object with that base has (the bare base counts as 0).
Crate, Crate_1, Crate_5 gives Crate_6 for a copy of any of them; a name nothing
else uses is kept (pasting after the original was deleted). Duplicate, array,
scatter, and paste share the rule, and a run of copies counts up. Pasted
descendants keep their names. `zenith_copy_naming` tests the pure function.

Copy and paste cost what is copied, not the scene. `copy` serializes only
the copied subtrees (`SceneDocument::serializeEntities`) and names the
collections they reference, which `paste` matches by name (same id and name,
then same name, else the reference is dropped). `paste` gives every copy a
fresh id (`Scene::reserveEntityId`), remaps entity references, sibling
orders (`remapChildOrder`), and parents inside the fragment (references to
entities outside it keep naming them), and creates the copies in the live
scene with `SceneDocument::createEntities`. Every other entity keeps its EnTT
handle and components, so in Play pasting or duplicating leaves the running
physics bodies of the rest of the scene alone; only the copies are marked for
physics. Prefab instance links and overrides, preserved components of
unregistered (module) types, and collections travel inside the copied
entities' document forms. `zenith_scoped_capture` covers scope parity for
every migrated operator, paste parity, handles, Play, sibling order, and
runtime meshes.

## Prefab file writes

Prefab files are shared project content, so the editor writes them at once
and makes the write part of history:

- **`PrefabWriteCommand`** (`document/PrefabWriteCommand`) is one undo step
  holding a file's content before and after (either may be absent: a created
  or removed file) and an `EntityDiffCommand` for the scene changes made with
  it. Undo and redo patch the scene, then rewrite the file atomically (a
  `.tmp` renamed over it, as `savePrefab` does) or remove it. Before writing,
  the command checks that the file holds the content of the side it leaves; a
  file that already holds the target is left alone (so the first execute,
  which `CommandHistory` runs on a write that already happened, writes
  nothing), and any other content fails the step with "<file> changed on disk
  since this step; undo skipped" without touching file or scene.
- **Recording it.** An `Undo` operator that writes a file sets
  `OperatorResult::undoStep` (an `EditorCommandFactory`); the runner passes it
  to `SceneEditing::commit(name, before, after, makeStep)`, which records the
  scene diff through it, even when the scene did not change. `prefab.apply`
  and `prefab.create_from_selection` do this.
- **Hooks and the ledger.** Every write reports the file's new content
  through `PrefabWriteHooks::fileWritten` (the operators get the session's
  through `EditorOperatorServices::prefabWritten`). The session records it in
  its `PrefabWriteLedger` (path to the content it last wrote or reconciled)
  and drops the game's cached copy (`GameServices::prefabs().invalidate`).
- **Outside changes.** The asset watcher's `prefabChanged` queues the file;
  once per frame, when no modal transform runs, the session classifies it
  with `classifyPrefabFileChange`. Content matching the ledger is the
  editor's own write (an apply, an undo or redo of one) and is ignored, so an
  undo never turns into a new step that would discard redo. Other content is
  an outside change: it is recorded in the ledger (repeated reports run
  once) and `refreshPrefabInstances` (`application/PrefabInstanceRefresh`)
  refreshes the open document's instances (the parked edit scene during
  Play) as one undo step, "Refresh <name> instances (changed on disk)", with
  an Info message. Each instance keeps its overrides against the file's
  previous content when the ledger knew it. The session seeds the ledger
  whenever instances enter a document: opening a scene, the startup scene,
  Recover, opening a prefab in prefab mode (its nested prefabs too), and
  `prefab.instantiate` report each instanced file's content through
  `PrefabWriteHooks::fileRead` (`notePrefabFilesRead`), which the ledger
  keeps unless it already knows the file. Only a file whose content could
  not be read falls back to each instance's persisted overrides (those of its
  last prefab operation), which the message mentions.
- **Parked documents.** In prefab mode (below) the same change also
  refreshes instances in every parked document, each as one step on its own
  history. The open prefab's own file is never refreshed under the user:
  changing it on disk while it is open only warns that Save writes over it.
- **Shared refresh.** Apply, the outside-change refresh, and a prefab edited
  as its own document all update instances through
  `PrefabAuthoring::refreshInstances(scene, reference, previous)` (or its
  `planRefresh` and `refresh` halves when the caller writes the file between
  them, as Apply does): overrides against the previous content, nested
  records the new content dropped recorded as removed, then a
  `KeepAddedObjects` refresh of each instance.
- **A prefab as a document.** `document/PrefabEditDocument` opens a prefab
  into an empty scene (`openPrefabDocument`: the root's id, the file's bytes,
  and its display name) and saves it back (`savePrefabDocument`: capture with
  `PrefabCaptureOptions::keepRootTransform`, atomic save, cache invalidation,
  the hooks, and linking added entities so their ids stay stable; it returns
  the before and after bytes for a `PrefabWriteCommand`). Opening and saving
  without edits keeps the file byte for byte.

## Prefab mode: the document stack

By default a prefab also opens in its own **Prefab workspace** tab (below the
document stack, which is what makes it a separate document): entering the
first prefab remembers the active workspace, switches to a workspace named
Prefab (created from `defaultWorkspaceFor(prefabWorkspaceName)` the first time
and saved with the others), and leaving the outermost prefab returns to the
remembered workspace. Nested prefabs stay in the Prefab workspace. The
`ui.prefab_workspace` preference (Preferences > Interface, on by default)
turns this off, which edits the prefab in the workspace that was open. The
engine renders and simulates one scene at a time, so this is a separate
document and tab, not a second OS window: an area of the Prefab workspace
shows the prefab, and the scene waits in the stack until Back.

A prefab opens as a document of its own (`prefab.edit`, see
[Operators](operators.md)) on top of the scene, like Blender's edit mode for
a linked file. The engine renders only one scene, so
`application/DocumentStack` parks the active document instead of keeping
several:

- **What is parked.** Each frame of the stack is one prefab edit: the prefab
  (`PrefabEditTarget`: reference, file, display name, root id, and what this
  visit saved) and the document it was opened over (`ParkedDocument`: the
  scene, moved out with `Scene::swap` as Play parks the edit scene; the
  `EditorDocument` state; the `CommandHistory` timeline; the selection; and
  each viewport's camera, scene-camera view, and local view
  (`ViewportPanel::takeDocumentView`)). The session's own members keep
  holding the active document, because operators, panels, and the undo keys
  refer to them by address: a switch exchanges their contents
  (`CommandHistory::exchangeTimeline` keeps revisions growing, so revision
  keyed caches such as the outliner's orphan data and the inspector's
  prefab overrides refresh, and markers stay meaningful). The session also
  forgets Adjust Last Operation, cancels the eyedropper, and frames the
  prefab in every open viewport; the outliner drops its range anchor when
  the scene's content generation changes.
- **Separate histories.** Edits in the prefab are ordinary
  `EntityDiffCommand` steps on the prefab's own history; undo there never
  reaches the scene's. The prefab document is untitled to `EditorDocument`
  (it saves no scene file); the breadcrumbs, prompts, and status bar name it
  by its display name.
- **Save.** File > Save (Ctrl S) writes the prefab (`DocumentStack::save`,
  through `savePrefabDocument`); the ids it gives entities added since
  opening are recorded as a "Save <name>" step, and the history and document
  become clean. Save As is disabled in prefab mode.
- **Back.** `prefab.exit_edit` (or the breadcrumbs' Back) asks about unsaved
  changes first (`PendingAction::LeavePrefab`: Save, Discard, or Cancel),
  then pops the frame and restores the viewports. If the prefab was saved
  during the visit, the document below refreshes its instances and records
  one step "Edit <name>": a `PrefabWriteCommand` whose undo puts back what
  the file held before the visit's first save, together with the old
  instances. Documents parked further down refresh theirs as a scene-only
  "Refresh <name> instances" step. If someone else changed the file after
  the last save, only the scene-only refresh is recorded, so undo never
  overwrites their change.
- **Nesting.** Opening a nested instance's prefab from inside prefab mode
  pushes another frame: `Level > room_07 > door_frame`. A prefab already open
  in the stack cannot be opened again.
- **Leaving for something else.** New, Open, Open Recent, Close, and the
  Project Hub leave every open prefab first, top down, prompting for each one
  with unsaved changes and then for the scene; cancelling any prompt cancels
  the action.
- **Gating.** Prefab mode is entered and left only in Edit mode with no file
  dialog pending and no modal tool running. Play and Simulate are disabled
  ("Return to the scene to play"), and a module hot reload waits for the
  stack to empty, since parked scenes hold module component storage the
  reload would not snapshot. The session unwinds the stack before the module
  unloads.
- **Moves and removals.** A moved or renamed open prefab follows its file
  (reference and path); parked scenes follow moved assets and their own file.
  A removed open prefab stays open with a warning, and Save writes the file
  again; a parked scene whose file was removed becomes untitled.
- **Autosave and recovery.** In prefab mode autosave writes the parked scene
  to the scene's recovery file when it has unsaved changes, and the open
  prefab to `<settings>/recovery.prefab-edit` (a scene document of the prefab
  edit) with `recovery.prefab-edit.json` beside it naming the prefab, its root
  id, and display name (`PrefabEditRecovery`). The crash save writes both.
  The Recover dialog mentions the prefab edit and reopens it in prefab mode
  over the recovered (or startup) scene, with unsaved changes. Saving the
  prefab or leaving it drops its autosave. Only the top prefab is autosaved;
  unsaved changes of a prefab parked under a nested one are not.
- **Known limit.** Undoing or redoing a prefab write refreshes the instances
  of the document whose history holds the step; documents parked beneath it
  keep what they showed until the file next changes or they are refreshed.

## Folder contents

`src/editor/document/`: Scene document state, persistence workflow, undo/redo
history, prefab authoring (`PrefabAuthoring`), undoable prefab file writes and
the write ledger (`PrefabWriteCommand`), prefabs opened as documents
(`PrefabEditDocument`), the scene's and a prefab edit's autosaves
(`EditorRecovery`, `PrefabEditRecovery`), per-scene editor data such as
the 3D cursor (`SceneEditorData`, a sidecar beside the scene file), and the
runtime-to-edit diff behind Apply to Edit Scene (`RuntimeApply`).
