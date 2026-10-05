#include "CollectionOperators.hpp"

#include "engine/scene/Scene.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace {
using ExecuteFunction = std::function<OperatorResult(OperatorContext&, OperatorProperties&)>;

template <typename Properties, typename Function> ExecuteFunction typed(Function run) {
    return [run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(context, *values);
    };
}

bool inEditMode(const OperatorContext& context) {
    return context.editMode();
}

bool hasSelection(const OperatorContext& context) {
    return context.editMode() && !context.selection.empty();
}

bool hasCollections(const OperatorContext& context) {
    return context.editMode() && !context.editing.scene().collections().empty();
}

bool hasSelectionAndCollections(const OperatorContext& context) {
    return hasSelection(context) && hasCollections(context);
}

// The collection named by an operator property, or a failure to return.
OperatorResult findCollection(const Scene& scene, uint32_t id, const SceneCollection*& collection) {
    collection = scene.collections().find(SceneCollectionId{id});
    return collection == nullptr ? OperatorResult::failed("Unknown collection " + std::to_string(id))
                                 : OperatorResult::finished();
}

Fields<CollectionProperties> collectionFields() {
    return std::move(Fields<CollectionProperties>().add("collection", &CollectionProperties::collection));
}

Operator newCollectionOperator() {
    return {.id = "collection.new",
            .label = "New Collection",
            .description = "Create a collection and link the selected objects to it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                NewCollectionProperties{},
                std::move(Fields<NewCollectionProperties>()
                              .add("name", &NewCollectionProperties::name)
                              .add("parent", &NewCollectionProperties::parent)
                              .tooltip("Id of the enclosing collection; 0 for a top-level collection")
                              .add("link_selection", &NewCollectionProperties::linkSelection)
                              .label("Link Selection")
                              .add("created", &NewCollectionProperties::created)
                              .readOnly())),
            .poll = inEditMode,
            .execute = typed<NewCollectionProperties>([](OperatorContext& context, NewCollectionProperties& values) {
                if (values.name.empty()) {
                    return OperatorResult::cancelled("Collections need a name");
                }
                Scene& scene = context.editing.scene();
                SceneCollections collections = scene.collections();
                SceneCollectionId created;
                if (CollectionResult result =
                        collections.create(values.name, SceneCollectionId{values.parent}, created);
                    !result) {
                    return OperatorResult::failed(result.error);
                }
                scene.setCollections(std::move(collections));
                values.created = static_cast<uint32_t>(created.value);
                if (values.linkSelection) {
                    for (const SceneEntityId id : context.selection.ids()) {
                        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                            linkToCollection(scene, entity, created);
                        }
                    }
                }
                return OperatorResult::finished();
            })};
}

Operator deleteCollectionOperator() {
    return {.id = "collection.delete",
            .label = "Delete Collection",
            .description = "Delete a collection; its objects and nested collections move to its parent",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(CollectionProperties{}, collectionFields()),
            .poll = hasCollections,
            .execute = typed<CollectionProperties>([](OperatorContext& context, CollectionProperties& values) {
                if (CollectionResult result =
                        deleteCollection(context.editing.scene(), SceneCollectionId{values.collection});
                    !result) {
                    return OperatorResult::failed(result.error);
                }
                return OperatorResult::finished();
            })};
}

std::string trimmed(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(whitespace) - first + 1));
}

Operator renameCollectionOperator() {
    return {
        .id = "collection.rename",
        .label = "Rename Collection",
        .description = "Rename a collection",
        .flags = OperatorFlags::Undo | OperatorFlags::Internal,
        .properties = OperatorProperties::make(RenameCollectionProperties{},
                                               std::move(Fields<RenameCollectionProperties>()
                                                             .add("collection", &RenameCollectionProperties::collection)
                                                             .add("name", &RenameCollectionProperties::name))),
        .poll = hasCollections,
        .execute = typed<RenameCollectionProperties>([](OperatorContext& context, RenameCollectionProperties& values) {
            Scene& scene = context.editing.scene();
            const SceneCollection* collection = nullptr;
            if (OperatorResult found = findCollection(scene, values.collection, collection); !found) {
                return found;
            }
            std::string name = trimmed(values.name);
            if (name.empty()) {
                return OperatorResult::cancelled("Collections need a name");
            }
            if (name == collection->name) {
                return OperatorResult::cancelled("The collection already has that name");
            }
            values.name = name;
            SceneCollections collections = scene.collections();
            if (CollectionResult result = collections.rename(collection->id, std::move(name)); !result) {
                return OperatorResult::failed(result.error);
            }
            scene.setCollections(std::move(collections));
            return OperatorResult::finished();
        })};
}

// Links or unlinks every selected object; cancelled when nothing changes.
Operator membershipOperator(std::string id, std::string label, std::string description, bool link) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(CollectionProperties{}, collectionFields()),
            .poll = hasSelectionAndCollections,
            .execute = typed<CollectionProperties>([link](OperatorContext& context, CollectionProperties& values) {
                Scene& scene = context.editing.scene();
                const SceneCollection* collection = nullptr;
                if (OperatorResult found = findCollection(scene, values.collection, collection); !found) {
                    return found;
                }
                bool changed = false;
                for (const SceneEntityId selected : context.selection.ids()) {
                    const Entity entity = scene.findEntity(selected);
                    if (entity == entt::null) {
                        continue;
                    }
                    const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity);
                    const bool member = membership != nullptr && membership->contains(collection->id);
                    if (member == link) {
                        continue;
                    }
                    const CollectionResult result = link ? linkToCollection(scene, entity, collection->id)
                                                         : unlinkFromCollection(scene, entity, collection->id);
                    if (!result) {
                        return OperatorResult::failed(result.error);
                    }
                    changed = true;
                }
                return changed ? OperatorResult::finished()
                               : OperatorResult::cancelled(link ? "The selection is already in the collection"
                                                                : "The selection is not in the collection");
            })};
}

Operator setFlagOperator() {
    return {.id = "collection.set_flag",
            .label = "Set Collection Flag",
            .description = "Show or hide a collection in viewports, or change whether it is selectable or rendered",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                CollectionFlagProperties{},
                std::move(Fields<CollectionFlagProperties>()
                              .add("collection", &CollectionFlagProperties::collection)
                              .add("flag", &CollectionFlagProperties::flag)
                              .options({{"visible", "Visible", static_cast<int64_t>(CollectionFlag::Visible)},
                                        {"selectable", "Selectable", static_cast<int64_t>(CollectionFlag::Selectable)},
                                        {"renderable", "Renderable", static_cast<int64_t>(CollectionFlag::Renderable)}})
                              .add("value", &CollectionFlagProperties::value))),
            .poll = hasCollections,
            .execute = typed<CollectionFlagProperties>([](OperatorContext& context, CollectionFlagProperties& values) {
                Scene& scene = context.editing.scene();
                const SceneCollection* collection = nullptr;
                if (OperatorResult found = findCollection(scene, values.collection, collection); !found) {
                    return found;
                }
                if (collection->flag(values.flag) == values.value) {
                    return OperatorResult::cancelled("The collection already has that setting");
                }
                SceneCollections collections = scene.collections();
                collections.setFlag(collection->id, values.flag, values.value);
                scene.setCollections(std::move(collections));
                return OperatorResult::finished();
            })};
}

Operator addCollectionInstanceOperator() {
    return {.id = "object.collection_instance_add",
            .label = "Add Collection Instance",
            .description = "Add an empty that draws a collection's objects at its transform",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties =
                OperatorProperties::make(CollectionInstanceProperties{},
                                         std::move(Fields<CollectionInstanceProperties>()
                                                       .add("collection", &CollectionInstanceProperties::collection)
                                                       .add("location", &CollectionInstanceProperties::location)
                                                       .drag(0.05f))),
            .poll = hasCollections,
            .execute =
                typed<CollectionInstanceProperties>([](OperatorContext& context, CollectionInstanceProperties& values) {
                    Scene& scene = context.editing.scene();
                    const SceneCollection* collection = nullptr;
                    if (OperatorResult found = findCollection(scene, values.collection, collection); !found) {
                        return found;
                    }
                    const Entity instance = scene.createEntity(collection->name);
                    scene.registry().emplace<CollectionInstanceComponent>(instance, collection->id);
                    scene.registry().get<TransformComponent>(instance).local[3] = glm::vec4(values.location, 1.0f);
                    scene.markTransformDirty(instance);
                    scene.markRenderableDirty(instance);
                    scene.updateWorldTransforms();
                    context.selection.selectOnly(scene.id(instance));
                    return OperatorResult::finished();
                })};
}
} // namespace

OperatorRegistrationResult registerCollectionOperators(OperatorRegistry& registry) {
    std::vector<Operator> operators;
    operators.push_back(newCollectionOperator());
    operators.push_back(deleteCollectionOperator());
    operators.push_back(renameCollectionOperator());
    operators.push_back(membershipOperator("collection.link", "Link to Collection",
                                           "Add the selected objects to a collection, keeping their other collections",
                                           true));
    operators.push_back(membershipOperator("collection.unlink", "Remove from Collection",
                                           "Remove the selected objects from a collection", false));
    operators.push_back(setFlagOperator());
    operators.push_back(addCollectionInstanceOperator());
    for (Operator& op : operators) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
