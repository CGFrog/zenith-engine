#pragma once

#include "editor/document/CommandHistory.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;
class EditorDiagnostics;
class EditorDocument;

// Records changes to the edited scene as undoable document commands. It holds
// references only, so the session builds one per frame from the objects it
// owns; every reference must outlive the SceneEditing value.
//
// Each edit captures a serialized snapshot before and after the change, but
// the snapshots are transient: commit records only the entities whose
// document form differs (EntityDiffCommand), and undo and redo patch those
// entities in place. An edit that knows which entities it changes (an
// inspector field, a transform drag) captures only those (captureEntities),
// so its cost follows the edit rather than the size of the scene. Entities an edit did not touch keep their EnTT
// handles; created or restored entities get new handles, so callers that hold entities across an edit should keep their
// stable SceneEntityId instead.
//
// A failure is reported to diagnostics and leaves the scene as the failed
// operation left it, matching the editor's existing command behavior.
//
// During Play and Simulate the session builds it for the runtime scene and
// passes the parked edit scene (EditorPlayMode::editScene) as editScene. It
// then edits live: scene() is the runtime scene, every edit stays applied
// there, and commit, perform, undo, and redo record nothing, so a runtime edit
// never enters the edit history. There is no runtime undo; Stop discards every
// runtime change. The one way back to the document is applyToEditScene, which
// records an ordinary command on the edit history against the edit scene.
class SceneEditing {
  public:
    // The document form of chosen entities before an edit (captureEntities).
    struct EntitySnapshot {
        std::vector<SceneEntityId> ids;
        std::vector<std::optional<std::string>> entities;
        // Entities the edit creates have this id or a higher one.
        SceneEntityId firstNewId;
        size_t entityCount{0};
    };

    // editScene is null in Edit mode; otherwise scene is the runtime scene and
    // editScene the edit scene Stop restores, which must outlive this value.
    SceneEditing(Scene& scene, AssetManager& assets, EditorDocument& document, CommandHistory& history,
                 EditorDiagnostics& diagnostics, Scene* editScene = nullptr);

    // The scene edits change: the edited document, or the runtime scene.
    Scene& scene() const;

    // Whether scene() is the runtime scene of Play or Simulate, whose edits
    // record no history and are lost on Stop.
    bool runtime() const {
        return m_editScene != nullptr;
    }

    // The edit scene parked while Play or Simulate runs; null in Edit mode.
    Scene* editScene() const {
        return m_editScene;
    }

    AssetManager& assets() const;
    const EditorDocument& document() const;
    EditorDiagnostics& diagnostics() const;
    // The history's revision (CommandHistory::revision): it changes with
    // every recorded edit, undo, redo, and document replacement, so views can
    // cache what they derive from the document.
    uint64_t historyRevision() const;

    // Serializes the scene into snapshot; returns false after reporting.
    bool capture(std::string& snapshot) const;
    // Serializes only entities, for an edit that changes nothing else: it may
    // create entities, but must not change or destroy any it does not list,
    // nor collection definitions. Returns false after reporting.
    bool captureEntities(std::span<const Entity> entities, EntitySnapshot& snapshot) const;
    // Records the change since before as one command. Edits sharing a
    // non-empty coalesceKey (for example a continuous drag) merge into one
    // undo step. A change that left the scene unchanged records nothing.
    // In the runtime scene the change stays applied, unrecorded; the changed
    // entities are marked dirty for the renderer, physics, and transforms.
    void commit(std::string name, std::string coalesceKey, std::string before) const;
    // As commit, for a caller that already captured the scene after the
    // change.
    void commit(std::string name, std::string coalesceKey, std::string_view before, std::string_view after) const;
    // As commit, for an edit captured with captureEntities: only the listed
    // entities and the ones the edit created are serialized again. An edit
    // that changed other entities is reported, and only those are recorded.
    void commit(std::string name, std::string coalesceKey, EntitySnapshot before) const;
    // Captures, runs mutation, and commits when it succeeds.
    void perform(std::string name, std::string coalesceKey, const std::function<SceneResult()>& mutation) const;
    // Records changes, whose before side must be the edit scene's current
    // state, as one undo step on the edit history and applies them to the edit
    // scene, leaving the runtime scene untouched. Stop then restores the
    // patched edit scene, so undo and redo after Stop act on it like any other
    // edit. Only valid while runtime(); returns false after reporting.
    bool applyToEditScene(std::string name, SceneChanges changes) const;

    // Undo and redo the edit history; they do nothing in the runtime scene.
    void undo() const;
    void redo() const;

    // The history's revision (CommandHistory::revision): it changes with every
    // recorded edit, undo, and redo, so views can cache what they derive from
    // the scene.
    uint64_t revision() const {
        return m_history.revision();
    }

    void syncDirtyState() const;
    EditorCommandContext commandContext() const;

  private:
    // Records changes the scene already shows as one command, or in the
    // runtime scene marks them (markRuntimeChanges).
    void record(std::string name, std::string coalesceKey, SceneChanges changes) const;
    // After a runtime edit: tells the scene's renderer, physics, and
    // transform caches what changed, as a recorded edit's
    // SceneDocument::applyChanges would, without rebuilding entities.
    void markRuntimeChanges(const SceneChanges& changes) const;

    Scene& m_scene;
    AssetManager& m_assets;
    EditorDocument& m_document;
    CommandHistory& m_history;
    EditorDiagnostics& m_diagnostics;
    Scene* m_editScene{nullptr};
};
