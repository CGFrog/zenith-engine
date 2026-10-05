#include "ModelOperators.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "engine/scene/Scene.hpp"

#include <utility>
#include <vector>

namespace {
// The absolute model file an asset property names.
std::filesystem::path modelFile(const OperatorContext& context, const std::string& asset) {
    const std::filesystem::path path = pathFromUtf8Text(asset);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return std::filesystem::absolute(context.editing.document().options().assetRoot / path).lexically_normal();
}

// Every entity below root, depth first.
std::vector<Entity> descendants(const Scene& scene, Entity root) {
    std::vector<Entity> found;
    std::vector<Entity> pending = scene.registry().get<RelationshipComponent>(root).children;
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        found.push_back(entity);
        const auto& children = scene.registry().get<RelationshipComponent>(entity).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return found;
}

bool isNodeOf(const Scene& scene, Entity entity, SceneEntityId root) {
    const auto* node = scene.registry().try_get<ModelNodeComponent>(entity);
    return node != nullptr && node->instanceRoot == root;
}

// Removes the model instance under root: user entities parented to its nodes
// move to root, then the nodes and the root's model components go. Returns
// how many entities moved.
SceneResult removeModelInstance(Scene& scene, Entity root, size_t& moved) {
    const SceneEntityId rootId = scene.id(root);
    std::vector<Entity> nodes;
    for (const Entity entity : descendants(scene, root)) {
        if (isNodeOf(scene, entity, rootId)) {
            nodes.push_back(entity);
        }
    }
    for (const Entity node : nodes) {
        // Copied: reparenting edits the list.
        const std::vector<Entity> children = scene.registry().get<RelationshipComponent>(node).children;
        for (const Entity child : children) {
            if (isNodeOf(scene, child, rootId)) {
                continue;
            }
            if (SceneResult result = scene.reparent(child, root, ReparentMode::PreserveWorldTransform); !result) {
                return result;
            }
            ++moved;
        }
    }
    for (const Entity node : nodes) {
        // Destroying a node destroys the nodes below it first.
        if (scene.registry().valid(node)) {
            scene.destroyEntity(node);
        }
    }
    scene.registry().remove<ModelComponent>(root);
    scene.registry().remove<AnimationComponent>(root);
    return {.success = true};
}

Operator modelReplaceOperator() {
    return {.id = "object.model_replace",
            .label = "Replace Model",
            .description = "Give an object another model asset, replacing the model entities below it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(ModelReplaceProperties{},
                                                   std::move(Fields<ModelReplaceProperties>()
                                                                 .add("entity", &ModelReplaceProperties::entity)
                                                                 .add("asset", &ModelReplaceProperties::asset)
                                                                 .asset("model"))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute =
                [](OperatorContext& context, OperatorProperties& properties) {
                    const auto* values = properties.get<ModelReplaceProperties>();
                    if (values == nullptr) {
                        return OperatorResult::failed("Operator properties have the wrong type");
                    }
                    if (values->asset.empty()) {
                        return OperatorResult::cancelled("Choose a model");
                    }
                    Scene& scene = context.editing.scene();
                    const Entity root = scene.findEntity(values->entity);
                    if (root == entt::null) {
                        return OperatorResult::failed("The object does not exist");
                    }
                    if (scene.registry().all_of<ModelNodeComponent>(root)) {
                        return OperatorResult::failed("A model's own node entities cannot hold another model");
                    }
                    AssetManager& assets = context.editing.assets();
                    const AssetLoadResult<ModelHandle> loaded = assets.loadModel(modelFile(context, values->asset));
                    if (!loaded) {
                        return OperatorResult::failed("Could not load the model: " + loaded.error);
                    }
                    if (const auto* current = scene.registry().try_get<ModelComponent>(root);
                        current != nullptr && current->model == loaded.handle) {
                        return OperatorResult::cancelled("The object already uses that model");
                    }
                    size_t moved = 0;
                    if (SceneResult removed = removeModelInstance(scene, root, moved); !removed) {
                        return OperatorResult::failed(removed.error);
                    }
                    if (SceneResult result = scene.instantiateModel(root, loaded.handle, assets); !result) {
                        return OperatorResult::failed(result.error);
                    }
                    scene.markTransformDirty(root);
                    scene.updateWorldTransforms();
                    if (moved > 0) {
                        context.editing.diagnostics().add(
                            EditorDiagnosticSeverity::Warning, "Model",
                            std::to_string(moved) + (moved == 1 ? " object was" : " objects were") +
                                " parented to a node of the replaced model and moved to the model's object.");
                    }
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerModelOperators(OperatorRegistry& registry) {
    return registry.add(modelReplaceOperator());
}

OperatorRegistrationResult registerModelOperatorFamily(OperatorRegistry& registry, const EditorOperatorServices&) {
    return registerModelOperators(registry);
}
