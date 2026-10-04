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
    SceneEntityId createdId;
    editing.perform("Create Entity", {}, [&] {
        const Entity created = scene.createEntity("Entity");
        createdId = scene.id(created);
        return parent == entt::null ? SceneResult{.success = true} : scene.reparent(created, parent);
    });
    return createdId;
}

void deleteEntities(const SceneEditing& editing, std::span<const SceneEntityId> entities, std::string commandName) {
    Scene& scene = editing.scene();
    const std::vector<SceneEntityId> targets(entities.begin(), entities.end());
    editing.perform(std::move(commandName), {}, [&] {
        for (SceneEntityId id : targets) {
            // Deleting a parent removes its children, so look each one up again.
            if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                // A prefab entity deleted from an instance stays deleted.
                recordPrefabEntityRemoval(scene, entity);
                scene.destroyEntity(entity);
            }
        }
        return SceneResult{.success = true};
    });
}

void deleteSelection(const SceneEditing& editing, Selection& selection) {
    if (selection.empty()) {
        return;
    }
    deleteEntities(editing, selection.ids(), "Delete");
    selection.clear();
}

bool duplicateSelectionUncommitted(const SceneEditing& editing, Selection& selection, std::string& before) {
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
    if (!editing.capture(before)) {
        return false;
    }
    std::vector<SceneEntityId> duplicates;
    Entity duplicateParent = entt::null;
    if (selection.size() == 1) {
        const Entity source = scene.findEntity(*selection.active());
        if (source != entt::null) {
            duplicateParent = scene.registry().get<RelationshipComponent>(source).parent;
        }
    }
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, scene, editing.assets(), options, duplicateParent, duplicates);
        !result) {
        editing.diagnostics().reportError("Duplicate failed: " + result.error);
        return false;
    }
    selection.assign(std::move(duplicates));
    return true;
}

void duplicateSelection(const SceneEditing& editing, Selection& selection) {
    std::string before;
    if (duplicateSelectionUncommitted(editing, selection, before)) {
        editing.commit("Duplicate Objects", {}, std::move(before));
    }
}

void placeModel(const SceneEditing& editing, Selection& selection, const std::filesystem::path& path, Entity parent,
                const glm::vec3& location) {
    Scene& scene = editing.scene();
    const AssetLoadResult<ModelHandle> loaded = editing.assets().loadModel(path);
    if (!loaded) {
        editing.diagnostics().reportError("Could not place model: " + loaded.error);
        return;
    }
    std::string before;
    if (!editing.capture(before)) {
        return;
    }
    const Entity root = scene.createEntity(path.stem().string());
    glm::vec4 local(location, 1.0f);
    if (parent != entt::null) {
        scene.setParent(root, parent);
        scene.updateWorldTransforms();
        local = glm::inverse(scene.registry().get<TransformComponent>(parent).world) * local;
    }
    scene.registry().get<TransformComponent>(root).local[3] = local;
    scene.markTransformDirty(root);
    if (const SceneResult result = scene.instantiateModel(root, loaded.handle, editing.assets()); !result) {
        scene.destroyEntity(root);
        editing.diagnostics().reportError("Could not place model: " + result.error);
        return;
    }
    const SceneEntityId id = scene.id(root);
    editing.commit("Place Model", {}, std::move(before));
    if (scene.findEntity(id) != entt::null) {
        selection.selectOnly(id);
    }
}
} // namespace EntityCommands
