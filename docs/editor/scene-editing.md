# Scene editing, history, and live inspection

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`SceneEditing` records only the entities an edit added, removed, or changed,
as an `EntityDiffCommand`, so history memory and undo/redo time scale with the
edit rather than the scene. `CommandHistory::storedBytes` reports what the
history keeps.

There are two ways to capture an edit:
- **The whole scene** (`capture`, then `commit` or `perform`). Use it when the
  edit's reach is unknown: structural edits and operators. `OperatorRunner`
  passes the after-capture it keeps for Adjust Last Operation to `commit`, so
  it is not serialized twice.
- **Only the entities the edit changes** (`captureEntities`, then `commit`
  with the `EntitySnapshot`). The inspector's name, transform and component
  edits and the viewport's transform widget and gizmo use it. Entities the
  edit creates are found by id and recorded too. Destroying an entity the
  snapshot did not list is reported, because undo could not bring it back.

In a 5,000-entity scene, recording an inspector edit costs about 1.7 ms this
way against 120 ms for the whole scene (`zenith_scene_editing_benchmark_run`). Opening or creating a document still replaces the whole scene and
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
an `Undo` operator's change in Info as lost on Stop; object and component
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
(which rebuilds the scene from its document) drops them and pastes at the top
level under a transient parent, saves and autosaves leave them out, and Play
enters without them (the parked edit scene keeps them for Stop). The
Hierarchy dims their rows with a "generated, not saved" tooltip.
`zenith_transient_editing` covers it.

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
  previous content when the ledger knew it; for a file the editor never wrote
  or saw change in this session, each keeps its persisted overrides (those of
  its last prefab operation), which the message mentions.
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

## Folder contents

`src/editor/document/`: Scene document state, persistence workflow, undo/redo
history, prefab authoring (`PrefabAuthoring`), undoable prefab file writes and
the write ledger (`PrefabWriteCommand`), prefabs opened as documents
(`PrefabEditDocument`), per-scene editor data such as
the 3D cursor (`SceneEditorData`, a sidecar beside the scene file), and the
runtime-to-edit diff behind Apply to Edit Scene (`RuntimeApply`).
