#include "EntityCommands.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/SceneFragment.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Prefab.hpp"

#include <utility>
#include <vector>

namespace EntityCommands {
SceneEntityId createEntity(const SceneEditing& editing, Entity parent) {
    Scene& scene = editing.scene();
    // The new entity, and the parent whose child order it joins.
    std::vector<SceneEntityId> parentId;
    if (parent != entt::null) {
        parentId.push_back(scene.id(parent));
    }
    SceneEditing::EntitySnapshot before;
    if (!editing.captureEntities(parentId, SceneEditing::CaptureReach::Entities,
                                 SceneEditing::CaptureCollections::Exclude, before)) {
        return {};
    }
    const Entity created = scene.createEntity("Entity");
    const SceneEntityId createdId = scene.id(created);
    if (parent != entt::null) {
        if (const SceneResult result = scene.reparent(created, parent); !result) {
            editing.diagnostics().reportError(result.error);
            return createdId;
        }
    }
    editing.commit("Create Entity", {}, std::move(before));
    return createdId;
}

void deleteEntities(const SceneEditing& editing, std::span<const SceneEntityId> entities, std::string commandName) {
    Scene& scene = editing.scene();
    const std::vector<SceneEntityId> targets(entities.begin(), entities.end());
    // Children go with their parent, and a prefab entity's removal is
    // recorded on its instance root, an ancestor.
    SceneEditing::EntitySnapshot before;
    if (!editing.captureEntities(targets, SceneEditing::CaptureReach::SubtreesAndAncestors,
                                 SceneEditing::CaptureCollections::Exclude, before)) {
        return;
    }
    for (SceneEntityId id : targets) {
        // Deleting a parent removes its children, so look each one up again.
        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
            // A prefab entity deleted from an instance stays deleted.
            recordPrefabEntityRemoval(scene, entity);
            scene.destroyEntity(entity);
        }
    }
    editing.commit(std::move(commandName), {}, std::move(before));
}

void deleteSelection(const SceneEditing& editing, Selection& selection) {
    if (selection.empty()) {
        return;
    }
    deleteEntities(editing, selection.ids(), "Delete");
    selection.clear();
}

bool duplicateSelectionUncommitted(const SceneEditing& editing, Selection& selection,
                                   SceneEditing::EntitySnapshot& before) {
    if (selection.empty()) {
        return false;
    }
    // The copies join the originals' parents, captured with them.
    if (!editing.captureEntities(selection.ids(), SceneEditing::CaptureReach::Entities,
                                 SceneEditing::CaptureCollections::Exclude, before)) {
        return false;
    }
    return duplicateSelectionUnrecorded(editing, selection);
}

bool duplicateSelectionUnrecorded(const SceneEditing& editing, Selection& selection) {
    if (selection.empty()) {
        return false;
    }
    Scene& scene = editing.scene();
    const SceneDocumentOptions& options = editing.document().options();
    std::string fragment;
    if (const SceneDocumentResult result =
            SceneFragment::copy(scene, editing.assets(), options, selection.ids(), fragment);
        !result) {
        editing.diagnostics().reportError("Duplicate failed: " + result.error);
        return false;
    }
    std::vector<SceneEntityId> sources;
    if (const SceneDocumentResult result = SceneFragment::roots(fragment, sources); !result) {
        editing.diagnostics().reportError("Duplicate failed: " + result.error);
        return false;
    }
    std::vector<SceneEntityId> duplicates;
    if (const SceneDocumentResult result =
            pasteBesideSources(editing, fragment, sources, PastedTransform::KeepSourceLocal, duplicates);
        !result) {
        editing.diagnostics().reportError("Duplicate failed: " + result.error);
        return false;
    }
    selection.assign(std::move(duplicates));
    return true;
}

SceneDocumentResult pasteBesideSources(const SceneEditing& editing, std::string_view fragment,
                                       std::span<const SceneEntityId> sources, PastedTransform transform,
                                       std::vector<SceneEntityId>& pasted) {
    Scene& scene = editing.scene();

    // Each source's parent and local transform, by id.
    struct Placement {
        SceneEntityId parent;
        glm::mat4 local{1.0f};
    };

    std::vector<Placement> placements;
    for (SceneEntityId id : sources) {
        Placement placement;
        if (const Entity source = scene.findEntity(id); source != entt::null) {
            const Entity parent = scene.registry().get<RelationshipComponent>(source).parent;
            if (parent != entt::null && !scene.isTransient(parent)) {
                placement.parent = scene.id(parent);
            }
            placement.local = scene.registry().get<TransformComponent>(source).local;
        }
        placements.push_back(placement);
    }
    // Pasted at the top level every copy lands on its fragment world
    // transform; each then joins its source's parent.
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, scene, editing.assets(), editing.document().options(), entt::null, pasted);
        !result) {
        return result;
    }
    for (size_t index = 0; index < pasted.size() && !placements.empty(); ++index) {
        const Placement& placement = placements[index % placements.size()];
        const Entity copy = scene.findEntity(pasted[index]);
        const Entity parent = placement.parent ? scene.findEntity(placement.parent) : entt::null;
        if (copy == entt::null || parent == entt::null) {
            continue;
        }
        // A copy its source's parent cannot take stays at the top level.
        const SceneResult joined =
            scene.reparent(copy, parent,
                           transform == PastedTransform::KeepWorld ? ReparentMode::PreserveWorldTransform
                                                                   : ReparentMode::PreserveLocalTransform);
        if (joined && transform == PastedTransform::KeepSourceLocal) {
            scene.registry().get<TransformComponent>(copy).local = placement.local;
            scene.markTransformDirty(copy);
        }
    }
    scene.updateWorldTransforms();
    return {.success = true};
}

void duplicateSelection(const SceneEditing& editing, Selection& selection) {
    SceneEditing::EntitySnapshot before;
    if (duplicateSelectionUncommitted(editing, selection, before)) {
        editing.commit("Duplicate Objects", {}, std::move(before));
    }
}

} // namespace EntityCommands
