#include "SceneEditing.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "engine/physics/PhysicsComponents.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <unordered_set>
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

SceneEditorData& SceneEditing::editorData() const {
    return m_document.editorData();
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
    std::vector<SceneEntityId> ids;
    ids.reserve(entities.size());
    for (const Entity entity : entities) {
        ids.push_back(m_scene.id(entity));
    }
    return captureEntities(ids, CaptureReach::Entities, CaptureCollections::Exclude, snapshot);
}

bool SceneEditing::captureEntities(std::span<const SceneEntityId> ids, CaptureReach reach,
                                   CaptureCollections collections, EntitySnapshot& snapshot) const {
    EntitySnapshot captured{.ids = withRelatives(m_scene, ids, reach),
                            .firstNewId = m_scene.nextEntityId(),
                            .entityCount = m_scene.registry().view<const SceneEntityIdComponent>().size()};
    for (const SceneEntityId id : captured.ids) {
        captured.existingCount += m_scene.findEntity(id) != entt::null ? 1 : 0;
    }
    if (const SceneDocumentResult result =
            SceneDocument::serializeEntities(m_scene, m_assets, m_document.options(), captured.ids, captured.entities);
        !result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    if (collections == CaptureCollections::Include) {
        captured.collections.emplace();
        SceneDocument::serializeCollections(m_scene, *captured.collections);
    }
    snapshot = std::move(captured);
    return true;
}

std::vector<SceneEntityId> SceneEditing::withRelatives(const Scene& scene, std::span<const SceneEntityId> ids,
                                                       CaptureReach reach) {
    std::vector<SceneEntityId> result;
    std::unordered_set<uint64_t> listed;
    const auto add = [&](SceneEntityId id) {
        if (id && listed.insert(id.value).second) {
            result.push_back(id);
        }
    };
    const entt::registry& registry = scene.registry();
    for (const SceneEntityId id : ids) {
        add(id);
        const Entity entity = scene.findEntity(id);
        if (entity == entt::null) {
            continue;
        }
        // A parent's document form holds its children's order, which
        // changes when a listed entity is removed, moved, or given a
        // sibling.
        if (const Entity parent = registry.get<RelationshipComponent>(entity).parent; parent != entt::null) {
            add(scene.id(parent));
        }
        if (reach == CaptureReach::Entities) {
            continue;
        }
        std::vector<Entity> pending(registry.get<RelationshipComponent>(entity).children);
        while (!pending.empty()) {
            const Entity child = pending.back();
            pending.pop_back();
            add(scene.id(child));
            const auto& children = registry.get<RelationshipComponent>(child).children;
            pending.insert(pending.end(), children.begin(), children.end());
        }
        if (reach == CaptureReach::SubtreesAndAncestors) {
            for (Entity parent = registry.get<RelationshipComponent>(entity).parent; parent != entt::null;
                 parent = registry.get<RelationshipComponent>(parent).parent) {
                add(scene.id(parent));
            }
        }
    }
    return result;
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

void SceneEditing::commit(std::string name, std::string_view before, std::string_view after,
                          const EditorCommandFactory& makeStep) const {
    SceneChanges changes;
    if (before != after) {
        if (const SceneDocumentResult result = SceneDocument::diff(before, after, changes); !result) {
            m_diagnostics.reportError("Could not record editor command: " + result.error);
            return;
        }
    }
    record(std::move(name), {}, std::move(changes), makeStep);
}

void SceneEditing::commit(std::string name, std::string coalesceKey, EntitySnapshot before) const {
    SceneChanges changes;
    if (changesSince(name, std::move(before), changes)) {
        record(std::move(name), std::move(coalesceKey), std::move(changes));
    }
}

bool SceneEditing::changesSince(std::string_view name, EntitySnapshot before, SceneChanges& changes,
                                EntitySnapshot* after) const {
    size_t stillExisting = 0;
    for (const SceneEntityId id : before.ids) {
        stillExisting += m_scene.findEntity(id) != entt::null ? 1 : 0;
    }
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
    std::vector<std::optional<std::string>> current;
    if (const SceneDocumentResult result =
            SceneDocument::serializeEntities(m_scene, m_assets, m_document.options(), before.ids, current);
        !result) {
        m_diagnostics.reportError("Could not record editor command: " + result.error);
        return false;
    }
    std::optional<std::string> collections;
    if (before.collections) {
        collections.emplace();
        SceneDocument::serializeCollections(m_scene, *collections);
    }
    // Counted by entity rather than document form, so a captured entity's
    // transient children (never serialized) count as captured when listed.
    if (m_scene.registry().view<const SceneEntityIdComponent>().size() + before.existingCount !=
        before.entityCount + created + stillExisting) {
        m_diagnostics.reportError(std::string(name) +
                                  " destroyed entities it did not capture; undo cannot bring them back");
    }
    if (after != nullptr) {
        *after = EntitySnapshot{.ids = before.ids,
                                .entities = current,
                                .collections = collections,
                                .firstNewId = m_scene.nextEntityId(),
                                .entityCount = m_scene.registry().view<const SceneEntityIdComponent>().size(),
                                .existingCount = stillExisting + created};
    }
    SceneChanges result;
    for (size_t index = 0; index < before.ids.size(); ++index) {
        if (before.entities[index] != current[index]) {
            result.entities.push_back({.id = before.ids[index],
                                       .before = std::move(before.entities[index]),
                                       .after = std::move(current[index])});
        }
    }
    std::ranges::sort(result.entities, {}, [](const SceneEntityChange& change) { return change.id.value; });
    if (before.collections && *before.collections != *collections) {
        result.collections =
            SceneCollectionsChange{.before = std::move(*before.collections), .after = std::move(*collections)};
    }
    changes = std::move(result);
    return true;
}

bool SceneEditing::unchangedSince(const EntitySnapshot& snapshot) const {
    if (m_scene.nextEntityId().value != snapshot.firstNewId.value) {
        return false;
    }
    std::vector<std::optional<std::string>> current;
    if (!SceneDocument::serializeEntities(m_scene, m_assets, m_document.options(), snapshot.ids, current)) {
        return false;
    }
    if (current != snapshot.entities) {
        return false;
    }
    if (snapshot.collections) {
        std::string collections;
        SceneDocument::serializeCollections(m_scene, collections);
        return collections == *snapshot.collections;
    }
    return true;
}

void SceneEditing::record(std::string name, std::string coalesceKey, SceneChanges changes,
                          const EditorCommandFactory& makeStep) const {
    if (runtime()) {
        // A runtime edit is already applied and is never recorded.
        if (!changes.empty()) {
            markRuntimeChanges(changes);
        }
        return;
    }
    if (changes.empty() && !makeStep) {
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
    std::unique_ptr<EditorCommand> step =
        makeStep ? makeStep(std::move(name), std::move(changes))
                 : std::make_unique<EntityDiffCommand>(std::move(name), std::move(changes), std::move(coalesceKey));
    EditorCommandContext command = commandContext();
    if (const EditorCommandResult result = m_history.execute(std::move(step), command); !result) {
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

bool SceneEditing::canUndo() const {
    return !runtime() && m_history.canUndo();
}

bool SceneEditing::canRedo() const {
    return !runtime() && m_history.canRedo();
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
