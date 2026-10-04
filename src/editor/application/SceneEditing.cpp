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

bool SceneEditing::captureEntities(std::span<const Entity> entities, EntitySnapshot& snapshot) const {
    EntitySnapshot captured{.firstNewId = m_scene.nextEntityId(),
                            .entityCount = m_scene.registry().view<const SceneEntityIdComponent>().size()};
    captured.ids.reserve(entities.size());
    for (const Entity entity : entities) {
        captured.ids.push_back(m_scene.id(entity));
    }
    if (const SceneDocumentResult result =
            SceneDocument::serializeEntities(m_scene, m_assets, m_document.options(), captured.ids, captured.entities);
        !result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    snapshot = std::move(captured);
    return true;
}

void SceneEditing::commit(std::string name, std::string coalesceKey, std::string before) const {
    std::string after;
    if (!capture(after)) {
        return;
    }
    commit(std::move(name), std::move(coalesceKey), before, after);
}

void SceneEditing::commit(std::string name, std::string coalesceKey, std::string_view before,
                          std::string_view after) const {
    if (before == after) {
        return;
    }
    SceneChanges changes;
    if (const SceneDocumentResult result = SceneDocument::diff(before, after, changes); !result) {
        m_diagnostics.reportError((runtime() ? "Could not inspect the runtime edit: "
                                             : "Could not record editor "
                                               "command: ") +
                                  result.error);
        return;
    }
    record(std::move(name), std::move(coalesceKey), std::move(changes));
}

void SceneEditing::commit(std::string name, std::string coalesceKey, EntitySnapshot before) const {
    // Entities the edit created are new on the after side.
    size_t created = 0;
    if (m_scene.nextEntityId().value != before.firstNewId.value) {
        for (const Entity entity : m_scene.registry().view<const SceneEntityIdComponent>()) {
            if (const SceneEntityId id = m_scene.id(entity); id.value >= before.firstNewId.value) {
                before.ids.push_back(id);
                before.entities.emplace_back();
                ++created;
            }
        }
    }
    std::vector<std::optional<std::string>> after;
    if (const SceneDocumentResult result =
            SceneDocument::serializeEntities(m_scene, m_assets, m_document.options(), before.ids, after);
        !result) {
        m_diagnostics.reportError("Could not record editor command: " + result.error);
        return;
    }
    SceneChanges changes;
    size_t destroyed = 0;
    for (size_t index = 0; index < before.ids.size(); ++index) {
        destroyed += before.entities[index] && !after[index] ? 1 : 0;
        if (before.entities[index] != after[index]) {
            changes.entities.push_back({.id = before.ids[index],
                                        .before = std::move(before.entities[index]),
                                        .after = std::move(after[index])});
        }
    }
    if (m_scene.registry().view<const SceneEntityIdComponent>().size() + destroyed != before.entityCount + created) {
        m_diagnostics.reportError(name + " destroyed entities it did not capture; undo cannot bring them back");
    }
    std::ranges::sort(changes.entities, {}, [](const SceneEntityChange& change) { return change.id.value; });
    record(std::move(name), std::move(coalesceKey), std::move(changes));
}

void SceneEditing::record(std::string name, std::string coalesceKey, SceneChanges changes) const {
    if (changes.empty()) {
        return;
    }
    if (runtime()) {
        // A runtime edit is already applied and is never recorded.
        markRuntimeChanges(changes);
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

void SceneEditing::markRuntimeChanges(const SceneChanges& changes) const {
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
