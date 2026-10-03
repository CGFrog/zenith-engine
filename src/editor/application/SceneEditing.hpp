#pragma once

#include "editor/document/CommandHistory.hpp"

#include <functional>
#include <string>

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
// entities in place. Entities an edit did not touch keep their EnTT handles;
// created or restored entities get new handles, so callers that hold
// entities across an edit should keep their stable SceneEntityId instead.
//
// A failure is reported to diagnostics and leaves the scene as the failed
// operation left it, matching the editor's existing command behavior.
class SceneEditing {
  public:
    SceneEditing(Scene& scene, AssetManager& assets, EditorDocument& document, CommandHistory& history,
                 EditorDiagnostics& diagnostics);

    Scene& scene() const;
    AssetManager& assets() const;
    const EditorDocument& document() const;
    EditorDiagnostics& diagnostics() const;
    // The history's revision (CommandHistory::revision): it changes with
    // every recorded edit, undo, redo, and document replacement, so views can
    // cache what they derive from the document.
    uint64_t historyRevision() const;

    // Serializes the scene into snapshot; returns false after reporting.
    bool capture(std::string& snapshot) const;
    // Records the change since before as one command. Edits sharing a
    // non-empty coalesceKey (for example a continuous drag) merge into one
    // undo step. A change that left the scene unchanged records nothing.
    void commit(std::string name, std::string coalesceKey, std::string before) const;
    // Captures, runs mutation, and commits when it succeeds.
    void perform(std::string name, std::string coalesceKey, const std::function<SceneResult()>& mutation) const;

    void undo() const;
    void redo() const;
    void syncDirtyState() const;
    EditorCommandContext commandContext() const;

  private:
    Scene& m_scene;
    AssetManager& m_assets;
    EditorDocument& m_document;
    CommandHistory& m_history;
    EditorDiagnostics& m_diagnostics;
};
