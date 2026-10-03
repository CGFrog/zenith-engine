#include "SceneEditing.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"

#include <memory>
#include <utility>
#include <vector>

SceneEditing::SceneEditing(Scene& scene, AssetManager& assets, EditorDocument& document, CommandHistory& history,
                           EditorDiagnostics& diagnostics)
    : m_scene(scene)
    , m_assets(assets)
    , m_document(document)
    , m_history(history)
    , m_diagnostics(diagnostics) {}

Scene& SceneEditing::scene() const {
    return m_scene;
}

AssetManager& SceneEditing::assets() const {
    return m_assets;
}

const EditorDocument& SceneEditing::document() const {
    return m_document;
}

EditorDiagnostics& SceneEditing::diagnostics() const {
    return m_diagnostics;
}

uint64_t SceneEditing::historyRevision() const {
    return m_history.revision();
}

bool SceneEditing::capture(std::string& snapshot) const {
    const SceneDocumentResult result = SceneDocument::serialize(m_scene, m_assets, m_document.options(), snapshot);
    if (!result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    return true;
}

void SceneEditing::commit(std::string name, std::string coalesceKey, std::string before) const {
    std::string after;
    if (!capture(after)) {
        return;
    }
    if (before == after) {
        return;
    }
    SceneChanges changes;
    if (const SceneDocumentResult result = SceneDocument::diff(before, after, changes); !result) {
        m_diagnostics.reportError("Could not record editor command: " + result.error);
        return;
    }
    if (changes.empty()) {
        return;
    }
    // History commands apply their own after state, so the changed entities
    // return to before first and the command reproduces the edit exactly,
    // just as redo will.
    if (const SceneDocumentResult result = SceneDocument::applyChanges(changes, SceneChangeDirection::Backward,
                                                                       m_assets, m_document.options(), m_scene);
        !result) {
        m_diagnostics.reportError("Could not prepare editor command: " + result.error);
        return;
    }
    EditorCommandContext command = commandContext();
    if (const EditorCommandResult result = m_history.execute(
            std::make_unique<EntityDiffCommand>(std::move(name), std::move(changes), std::move(coalesceKey)), command);
        !result) {
        m_diagnostics.reportError(result.error);
        return;
    }
    syncDirtyState();
}

void SceneEditing::perform(std::string name, std::string coalesceKey,
                           const std::function<SceneResult()>& mutation) const {
    std::string before;
    if (!capture(before)) {
        return;
    }
    if (const SceneResult result = mutation(); !result) {
        m_diagnostics.reportError(result.error);
        return;
    }
    commit(std::move(name), std::move(coalesceKey), std::move(before));
}

void SceneEditing::undo() const {
    if (!m_history.canUndo()) {
        return;
    }
    EditorCommandContext command = commandContext();
    if (const EditorCommandResult result = m_history.undo(command); !result) {
        m_diagnostics.reportError(result.error);
    }
    syncDirtyState();
}

void SceneEditing::redo() const {
    if (!m_history.canRedo()) {
        return;
    }
    EditorCommandContext command = commandContext();
    if (const EditorCommandResult result = m_history.redo(command); !result) {
        m_diagnostics.reportError(result.error);
    }
    syncDirtyState();
}

void SceneEditing::syncDirtyState() const {
    m_document.setDirty(m_history.dirty());
}

EditorCommandContext SceneEditing::commandContext() const {
    return {m_scene, m_assets, m_document.options()};
}
