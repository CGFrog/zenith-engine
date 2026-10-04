#include "SceneEditing.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "engine/physics/PhysicsComponents.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <utility>
#include <vector>

SceneEditing::SceneEditing(Scene& scene, AssetManager& assets, EditorDocument& document, CommandHistory& history,
                           EditorDiagnostics& diagnostics, Scene* editScene)
    : m_scene(scene)
    , m_assets(assets)
    , m_document(document)
    , m_history(history)
    , m_diagnostics(diagnostics)
    , m_editScene(editScene) {}

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
    if (runtime()) {
        // A runtime edit is already applied and is never recorded.
        markRuntimeChanges(before);
        return;
    }
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

void SceneEditing::markRuntimeChanges(std::string_view before) const {
    std::string after;
    if (!capture(after) || before == after) {
        return;
    }
    SceneChanges changes;
    if (const SceneDocumentResult result = SceneDocument::diff(before, after, changes); !result) {
        m_diagnostics.reportError("Could not inspect the runtime edit: " + result.error);
        return;
    }
    // What applyChanges marks for a recorded edit, without rebuilding the
    // entities (which would drop runtime-only state such as physics bodies).
    const auto hadPhysics = [](const std::optional<std::string>& entity) {
        const nlohmann::json json = entity ? nlohmann::json::parse(*entity, nullptr, false) : nlohmann::json();
        const auto components = json.is_object() ? json.find("components") : json.end();
        return components != json.end() && components->is_object() &&
               (components->contains("collider") || components->contains("rigid_body"));
    };
    Entity changed = entt::null;
    for (const SceneEntityChange& change : changes.entities) {
        const Entity entity = m_scene.findEntity(change.id);
        if (entity == entt::null) {
            continue;
        }
        changed = entity;
        m_scene.markTransformDirty(entity);
        if (hadPhysics(change.before) || m_scene.registry().any_of<ColliderComponent, RigidBodyComponent>(entity)) {
            m_scene.markPhysicsDirty(entity);
        }
    }
    if (changed != entt::null) {
        // Scene-wide revisions: one bump covers every changed entity.
        m_scene.markRenderableDirty(changed);
        m_scene.markLightDirty(changed);
        m_scene.markCameraDirty(changed);
        m_scene.markSkinDirty(changed);
        m_scene.markMaterialDirty(changed);
    }
    m_scene.updateWorldTransforms();
}

bool SceneEditing::applyToEditScene(std::string name, SceneChanges changes) const {
    if (m_editScene == nullptr) {
        m_diagnostics.reportError("Apply to Edit Scene needs Play or Simulate");
        return false;
    }
    // The command applies itself to the edit scene; the runtime scene, which
    // m_scene names, is never touched.
    EditorCommandContext editContext{*m_editScene, m_assets, m_document.options()};
    if (const EditorCommandResult result =
            m_history.execute(std::make_unique<EntityDiffCommand>(std::move(name), std::move(changes)), editContext);
        !result) {
        m_diagnostics.reportError("Could not apply to the edit scene: " + result.error);
        return false;
    }
    syncDirtyState();
    return true;
}

void SceneEditing::undo() const {
    if (runtime() || !m_history.canUndo()) {
        return;
    }
    EditorCommandContext command = commandContext();
    if (const EditorCommandResult result = m_history.undo(command); !result) {
        m_diagnostics.reportError(result.error);
    }
    syncDirtyState();
}

void SceneEditing::redo() const {
    if (runtime() || !m_history.canRedo()) {
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
