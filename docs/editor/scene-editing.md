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

## Folder contents

`src/editor/document/`: Scene document state, persistence workflow, undo/redo
history, prefab authoring (`PrefabAuthoring`), per-scene editor data such as
the 3D cursor (`SceneEditorData`, a sidecar beside the scene file), and the
runtime-to-edit diff behind Apply to Edit Scene (`RuntimeApply`).
