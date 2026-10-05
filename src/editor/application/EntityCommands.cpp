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
    std::vector<SceneEntityId> sources;
    if (const SceneDocumentResult result = SceneFragment::roots(fragment, sources); !result) {
        editing.diagnostics().reportError("Duplicate failed: " + result.error);
        return false;
    }
    if (!editing.capture(before)) {
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

    // Each source's parent and local transform, by id: paste rebuilds the
    // scene, so entity handles do not survive it.
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
    std::string before;
    if (duplicateSelectionUncommitted(editing, selection, before)) {
        editing.commit("Duplicate Objects", {}, std::move(before));
    }
}

} // namespace EntityCommands
