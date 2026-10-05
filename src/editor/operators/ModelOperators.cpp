#include "ModelOperators.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>
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

std::string entityName(const Scene& scene, Entity entity) {
    const auto* name = scene.registry().try_get<NameComponent>(entity);
    return name != nullptr && !name->name.empty() ? name->name : std::string("An object");
}

// Whether node is a joint of a skin of its model, so other entities' skins
// follow it through its Model Node.
bool isSkeletonJoint(const AssetManager& assets, const ModelNodeComponent& node) {
    const ModelAsset* model = assets.model(node.model);
    return model != nullptr && std::ranges::any_of(model->skins, [&](const SkinAsset& skin) {
               return std::ranges::find(skin.joints, node.nodeIndex) != skin.joints.end();
           });
}

// The mesh being assigned: its handle, where it comes from, and its asset.
struct AssignedMesh {
    MeshHandle handle;
    std::filesystem::path model;
    uint32_t index{0};
    const MeshAsset* asset{nullptr};
};

// Whether a node of the model that skin skins with draws the assigned mesh
// with that same skin, so the skin's joint palette fits the new mesh.
bool skinFits(const AssetManager& assets, const SkinComponent& skin, const AssignedMesh& mesh) {
    const ModelAsset* model = assets.model(skin.model);
    const std::optional<ModelAssetSource> source = assets.modelSource(skin.model);
    if (model == nullptr || !source || source->path != mesh.model) {
        return false;
    }
    return std::ranges::any_of(model->nodes, [&](const ModelNode& node) {
        const std::optional<MeshAssetSource> nodeMesh = assets.meshSource(node.mesh);
        return node.skin == static_cast<int32_t>(skin.skinIndex) && nodeMesh && nodeMesh->meshIndex == mesh.index;
    });
}

// The morph weights a node of the mesh's model starts with, or zeros.
std::vector<float> defaultMorphWeights(const AssetManager& assets, const AssignedMesh& mesh, ModelHandle model) {
    const size_t targets = mesh.asset->morphTargets.size();
    if (const ModelAsset* asset = assets.model(model)) {
        for (const ModelNode& node : asset->nodes) {
            if (node.mesh == mesh.handle && node.morphWeights.size() == targets) {
                return node.morphWeights;
            }
        }
    }
    return std::vector<float>(targets, 0.0f);
}

// What applying the mesh to one entity did, for the diagnostics.
struct MeshAssignOutcome {
    bool detached{false};
    bool skinRemoved{false};
};

MeshAssignOutcome assignMesh(Scene& scene, const AssetManager& assets, Entity entity, const AssignedMesh& mesh,
                             ModelHandle model, bool updateCollider) {
    entt::registry& registry = scene.registry();
    MeshAssignOutcome outcome;
    const auto* previous = registry.try_get<MeshRendererComponent>(entity);
    const MeshHandle oldMesh = previous != nullptr ? previous->mesh : MeshHandle{};
    scene.attachMesh(entity, mesh.handle);
    scene.markRenderableDirty(entity);

    if (registry.remove<ModelNodeComponent>(entity) > 0) {
        outcome.detached = true;
    }
    if (const auto* skin = registry.try_get<SkinComponent>(entity); skin != nullptr && !skinFits(assets, *skin, mesh)) {
        registry.remove<SkinComponent>(entity);
        outcome.skinRemoved = true;
        scene.markSkinDirty(entity);
    }
    const size_t targets = mesh.asset->morphTargets.size();
    if (auto* weights = registry.try_get<MorphWeightsComponent>(entity); weights != nullptr && targets == 0) {
        registry.remove<MorphWeightsComponent>(entity);
        scene.markSkinDirty(entity);
    } else if (targets > 0 && (weights == nullptr || weights->weights.size() != targets)) {
        registry.emplace_or_replace<MorphWeightsComponent>(entity, defaultMorphWeights(assets, mesh, model));
        scene.markSkinDirty(entity);
    }

    if (auto* collider = registry.try_get<ColliderComponent>(entity); updateCollider && collider != nullptr) {
        if (auto* triangles = std::get_if<TriangleMeshCollider>(&collider->shape);
            triangles != nullptr && oldMesh && triangles->mesh == oldMesh) {
            triangles->mesh = mesh.handle;
            scene.markPhysicsDirty(entity);
        }
    }
    return outcome;
}

void reportMeshAssignment(EditorDiagnostics& diagnostics, size_t detached, size_t skinsRemoved) {
    if (detached > 0) {
        diagnostics.add(EditorDiagnosticSeverity::Info, "Mesh",
                        std::to_string(detached) + (detached == 1 ? " object" : " objects") +
                            " left its model instance: the model's animation no longer moves " +
                            (detached == 1 ? "it." : "them."));
    }
    if (skinsRemoved > 0) {
        diagnostics.add(EditorDiagnosticSeverity::Info, "Mesh",
                        "Removed the skin of " + std::to_string(skinsRemoved) +
                            (skinsRemoved == 1 ? " object" : " objects") +
                            ": the new mesh is not drawn with that skin, so it is drawn static.");
    }
}

Operator meshAssignOperator() {
    using P = MeshAssignProperties;
    return {.id = "object.mesh_assign",
            .label = "Assign Mesh",
            .description = "Give objects one mesh of a model, adding a Mesh Renderer where they have none",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                P{}, std::move(Fields<P>()
                                   .list("entities", &P::entities,
                                         Fields<MeshAssignTarget>().add("entity", &MeshAssignTarget::entity))
                                   .add("asset", &P::asset)
                                   .asset("model")
                                   .add("mesh", &P::mesh)
                                   .add("update_collider", &P::updateCollider)
                                   .tooltip("Also point a triangle-mesh collider that used the previous mesh at the "
                                            "new one"))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute =
                [](OperatorContext& context, OperatorProperties& properties) {
                    const auto* values = properties.get<P>();
                    if (values == nullptr) {
                        return OperatorResult::failed("Operator properties have the wrong type");
                    }
                    if (values->asset.empty()) {
                        return OperatorResult::cancelled("Choose a model");
                    }
                    if (values->entities.empty()) {
                        return OperatorResult::cancelled("Select an object");
                    }
                    Scene& scene = context.editing.scene();
                    AssetManager& assets = context.editing.assets();
                    const std::filesystem::path file = modelFile(context, values->asset);
                    const AssetLoadResult<MeshHandle> loaded = assets.loadMesh(file, values->mesh);
                    if (!loaded) {
                        return OperatorResult::failed("Could not load the mesh: " + loaded.error);
                    }
                    const AssetLoadResult<ModelHandle> model = assets.loadModel(file);
                    const std::optional<MeshAssetSource> source = assets.meshSource(loaded.handle);
                    const AssignedMesh mesh{.handle = loaded.handle,
                                            .model = source ? source->path : file,
                                            .index = values->mesh,
                                            .asset = assets.mesh(loaded.handle)};
                    if (mesh.asset == nullptr) {
                        return OperatorResult::failed("The mesh is not loaded");
                    }

                    // Validated first, so a refusal changes nothing.
                    std::vector<Entity> targets;
                    for (const MeshAssignTarget& target : values->entities) {
                        const Entity entity = scene.findEntity(target.entity);
                        if (entity == entt::null) {
                            return OperatorResult::failed("An object to give the mesh does not exist");
                        }
                        if (const auto* node = scene.registry().try_get<ModelNodeComponent>(entity);
                            node != nullptr && isSkeletonJoint(assets, *node)) {
                            return OperatorResult::failed(entityName(scene, entity) +
                                                          " is a joint of its model's skeleton, so its mesh "
                                                          "cannot be replaced");
                        }
                        const auto* renderer = scene.registry().try_get<MeshRendererComponent>(entity);
                        if ((renderer == nullptr || renderer->mesh != mesh.handle) &&
                            std::ranges::find(targets, entity) == targets.end()) {
                            targets.push_back(entity);
                        }
                    }
                    if (targets.empty()) {
                        return OperatorResult::cancelled("The objects already use that mesh");
                    }

                    size_t detached = 0;
                    size_t skinsRemoved = 0;
                    for (const Entity entity : targets) {
                        const MeshAssignOutcome outcome =
                            assignMesh(scene, assets, entity, mesh, model.handle, values->updateCollider);
                        detached += outcome.detached ? 1 : 0;
                        skinsRemoved += outcome.skinRemoved ? 1 : 0;
                    }
                    reportMeshAssignment(context.editing.diagnostics(), detached, skinsRemoved);
                    return OperatorResult::finished();
                },
            .invokeDefaults =
                [](const OperatorContext& context, OperatorProperties& properties) {
                    if (auto* values = properties.get<P>(); values != nullptr && values->entities.empty()) {
                        for (const SceneEntityId id : context.selection.ids()) {
                            values->entities.push_back({.entity = id});
                        }
                    }
                }};
}
} // namespace

OperatorRegistrationResult registerModelOperators(OperatorRegistry& registry) {
    return addOperators(registry, {modelReplaceOperator(), meshAssignOperator()});
}

OperatorRegistrationResult registerModelOperatorFamily(OperatorRegistry& registry, const EditorOperatorServices&) {
    return registerModelOperators(registry);
}
