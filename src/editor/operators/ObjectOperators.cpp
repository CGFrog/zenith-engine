#include "ObjectOperators.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/operators/CollectionOperators.hpp"
#include "editor/operators/OutlinerOperators.hpp"
#include "editor/operators/VisibilityOperators.hpp"
#include "editor/project/IrradianceVolumeBaker.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {
using ExecuteFunction = std::function<OperatorResult(OperatorContext&, OperatorProperties&)>;

// Adapts an execute function taking the operator's property struct.
template <typename Properties, typename Function> ExecuteFunction typed(Function run) {
    return [run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(context, *values);
    };
}

// Object operators edit whichever scene the context edits: the document in
// Edit mode, or, live, the runtime scene of Play and Simulate, where nothing
// is recorded and Stop discards the change (SceneEditing::runtime).
bool sceneEditable(const OperatorContext&) {
    return true;
}

bool hasSelection(const OperatorContext& context) {
    return !context.selection.empty();
}

Entity activeEntity(const OperatorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    return active ? context.editing.scene().findEntity(*active) : entt::null;
}

bool hasActive(const OperatorContext& context) {
    return activeEntity(context) != entt::null;
}

void placeEntity(Scene& scene, Entity entity, const glm::vec3& location, float size) {
    glm::mat4& local = scene.registry().get<TransformComponent>(entity).local;
    for (int column = 0; column < 3; ++column) {
        local[column] *= size;
    }
    local[3] = glm::vec4(location, 1.0f);
    scene.markTransformDirty(entity);
    scene.updateWorldTransforms();
}

OperatorResult selectCreated(OperatorContext& context, Entity entity) {
    context.selection.selectOnly(context.editing.scene().id(entity));
    return OperatorResult::finished();
}

Fields<AddObjectProperties> addObjectFields() {
    return std::move(Fields<AddObjectProperties>().add("location", &AddObjectProperties::location).drag(0.05f));
}

// Sun, environment, fog, and empty differ only in the function that creates them.
Operator addObjectOperator(std::string id, std::string label, std::string description, Entity (*create)(Scene&)) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(AddObjectProperties{}, addObjectFields()),
            .poll = sceneEditable,
            .execute = typed<AddObjectProperties>([create](OperatorContext& context, AddObjectProperties& values) {
                Scene& scene = context.editing.scene();
                const Entity entity = create(scene);
                placeEntity(scene, entity, values.location, 1.0f);
                return selectCreated(context, entity);
            })};
}

Operator addCubeOperator() {
    return {.id = "object.add_cube",
            .label = "Add Cube",
            .description = "Add the project's default cube mesh",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(AddCubeProperties{},
                                                   std::move(Fields<AddCubeProperties>()
                                                                 .add("location", &AddCubeProperties::location)
                                                                 .drag(0.05f)
                                                                 .add("size", &AddCubeProperties::size)
                                                                 .drag(0.01f, 0.001, 1000.0))),
            .poll = sceneEditable,
            .execute = typed<AddCubeProperties>([](OperatorContext& context, AddCubeProperties& values) {
                Scene& scene = context.editing.scene();
                Entity cube = entt::null;
                if (const SceneResult result =
                        addCube(scene, context.editing.assets(), context.editing.document().options().assetRoot, cube);
                    !result) {
                    return OperatorResult::failed(result.error);
                }
                placeEntity(scene, cube, values.location, values.size);
                return selectCreated(context, cube);
            })};
}

Operator createEntityOperator() {
    return {.id = "object.create_entity",
            .label = "Create Entity",
            .description = "Create an empty entity named \"Entity\"",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                CreateEntityProperties{},
                std::move(Fields<CreateEntityProperties>()
                              .add("placement", &CreateEntityProperties::placement)
                              .options({{"root", "Scene root", static_cast<int64_t>(NewEntityPlacement::Root)},
                                        {"child", "Child of active", static_cast<int64_t>(NewEntityPlacement::Child)},
                                        {"sibling", "Sibling of active",
                                         static_cast<int64_t>(NewEntityPlacement::Sibling)}}))),
            .poll = sceneEditable,
            .execute = typed<CreateEntityProperties>([](OperatorContext& context, CreateEntityProperties& values) {
                Scene& scene = context.editing.scene();
                const Entity active = activeEntity(context);
                Entity parent = entt::null;
                if (values.placement == NewEntityPlacement::Child && context.selection.size() == 1) {
                    parent = active;
                } else if (values.placement == NewEntityPlacement::Sibling && active != entt::null) {
                    parent = scene.registry().get<RelationshipComponent>(active).parent;
                }
                const Entity created = scene.createEntity("Entity");
                if (parent != entt::null) {
                    if (const SceneResult result = scene.reparent(created, parent); !result) {
                        return OperatorResult::failed(result.error);
                    }
                }
                return selectCreated(context, created);
            })};
}

Operator duplicateOperator() {
    return {.id = "object.duplicate",
            .label = "Duplicate Objects",
            .description = "Copy the selected objects and their children, and select the copies",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                DuplicateProperties{},
                std::move(Fields<DuplicateProperties>().add("offset", &DuplicateProperties::offset).drag(0.05f))),
            .poll = hasSelection,
            .execute = typed<DuplicateProperties>([](OperatorContext& context, DuplicateProperties& values) {
                std::string ignoredBefore;
                if (!EntityCommands::duplicateSelectionUncommitted(context.editing, context.selection, ignoredBefore)) {
                    // The failure was already reported.
                    return OperatorResult::cancelled("Nothing was duplicated");
                }
                Scene& scene = context.editing.scene();
                for (SceneEntityId id : context.selection.ids()) {
                    if (const Entity copy = scene.findEntity(id); copy != entt::null) {
                        scene.registry().get<TransformComponent>(copy).local[3] += glm::vec4(values.offset, 0.0f);
                        scene.markTransformDirty(copy);
                    }
                }
                scene.updateWorldTransforms();
                return OperatorResult::finished();
            })};
}

Operator deleteOperator() {
    return {.id = "object.delete",
            .label = "Delete",
            .description = "Delete the selected objects and their children",
            .flags = OperatorFlags::Undo,
            .poll = hasSelection,
            .execute = [](OperatorContext& context, OperatorProperties&) {
                Scene& scene = context.editing.scene();
                for (SceneEntityId id : context.selection.ids()) {
                    // Deleting a parent removes its children, so look each one up again.
                    if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                        // A prefab entity deleted from an instance stays deleted.
                        recordPrefabEntityRemoval(scene, entity);
                        scene.destroyEntity(entity);
                    }
                }
                context.selection.clear();
                return OperatorResult::finished();
            }};
}

Operator selectAllOperator() {
    return {.id = "object.select_all",
            .label = "Select All",
            .description = "Select, deselect, or invert the selection of every object",
            .properties = OperatorProperties::make(
                SelectAllProperties{},
                std::move(Fields<SelectAllProperties>()
                              .add("action", &SelectAllProperties::action)
                              .options({{"select", "Select", static_cast<int64_t>(SelectAllAction::Select)},
                                        {"deselect", "Deselect", static_cast<int64_t>(SelectAllAction::Deselect)},
                                        {"invert", "Invert", static_cast<int64_t>(SelectAllAction::Invert)}}))),
            .poll = sceneEditable,
            .execute = typed<SelectAllProperties>([](OperatorContext& context, SelectAllProperties& values) {
                switch (values.action) {
                case SelectAllAction::Select:
                    context.selection.selectAll(context.editing.scene());
                    break;
                case SelectAllAction::Deselect:
                    context.selection.clear();
                    break;
                case SelectAllAction::Invert:
                    context.selection.invert(context.editing.scene());
                    break;
                }
                return OperatorResult::finished();
            })};
}

Operator clearTransformOperator(std::string id, std::string label, TransformPart part) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = "Reset this part of the selected objects' local transforms",
            .flags = OperatorFlags::Undo,
            .poll = hasSelection,
            .execute = [part](OperatorContext& context, OperatorProperties&) {
                Scene& scene = context.editing.scene();
                for (SceneEntityId id : context.selection.ids()) {
                    if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                        glm::mat4& local = scene.registry().get<TransformComponent>(entity).local;
                        local = clearTransformPart(local, part);
                        scene.markTransformDirty(entity);
                    }
                }
                scene.updateWorldTransforms();
                return OperatorResult::finished();
            }};
}

Operator renameOperator() {
    return {.id = "object.rename",
            .label = "Rename",
            .description = "Rename the active object",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                RenameProperties{}, std::move(Fields<RenameProperties>().add("name", &RenameProperties::name))),
            .poll = hasActive,
            .execute = typed<RenameProperties>([](OperatorContext& context, RenameProperties& values) {
                if (values.name.empty()) {
                    return OperatorResult::cancelled("Objects need a name");
                }
                context.editing.scene().registry().get<NameComponent>(activeEntity(context)).name = values.name;
                return OperatorResult::finished();
            })};
}

Fields<ComponentProperties> componentFields() {
    return std::move(Fields<ComponentProperties>().add("component", &ComponentProperties::component));
}

Operator addComponentOperator() {
    return {.id = "object.component_add",
            .label = "Add Component",
            .description = "Add a component with default values to the active object",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasActive,
            .execute = typed<ComponentProperties>([](OperatorContext& context, ComponentProperties& values) {
                Scene& scene = context.editing.scene();
                const ComponentType* type =
                    componentTypesFor(context.editing.document().options()).find(values.component);
                if (type == nullptr || type->internal()) {
                    return OperatorResult::failed("Unknown component type '" + values.component + "'");
                }
                const Entity entity = activeEntity(context);
                if (type->has(scene, entity)) {
                    return OperatorResult::cancelled(type->displayName() + " is already present");
                }
                type->addDefault(scene, entity);
                return OperatorResult::finished();
            })};
}

// "Create Empty with Component X": an entity named after the component.
Operator addEmptyWithOperator() {
    return {.id = "object.add_empty_with",
            .label = "Add Empty with Component",
            .description = "Add an entity with one component, named after it, and select it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties = OperatorProperties::make(AddEmptyWithProperties{},
                                                   std::move(Fields<AddEmptyWithProperties>()
                                                                 .add("component", &AddEmptyWithProperties::component)
                                                                 .add("location", &AddEmptyWithProperties::location)
                                                                 .drag(0.05f))),
            .poll = sceneEditable,
            .execute = typed<AddEmptyWithProperties>([](OperatorContext& context, AddEmptyWithProperties& values) {
                const ComponentType* type =
                    componentTypesFor(context.editing.document().options()).find(values.component);
                if (type == nullptr || type->internal()) {
                    return OperatorResult::failed("Unknown component type '" + values.component + "'");
                }
                Scene& scene = context.editing.scene();
                const Entity entity = scene.createEntity(type->displayName());
                type->addDefault(scene, entity);
                placeEntity(scene, entity, values.location, 1.0f);
                return selectCreated(context, entity);
            })};
}

// Removes a registered component, or the preserved data of a component type
// that is not registered (for example from an unloaded game module), from one
// entity. Returns whether the entity had it.
bool removeComponent(Scene& scene, const ComponentType* type, Entity entity, const std::string& key) {
    if (type != nullptr && type->has(scene, entity)) {
        type->remove(scene, entity);
        return true;
    }
    auto* preserved = scene.registry().try_get<PreservedComponentsComponent>(entity);
    if (preserved == nullptr ||
        std::erase_if(preserved->components, [&key](const auto& entry) { return entry.first == key; }) == 0) {
        return false;
    }
    if (preserved->components.empty()) {
        scene.registry().remove<PreservedComponentsComponent>(entity);
    }
    return true;
}

Operator removeComponentOperator() {
    return {.id = "object.component_remove",
            .label = "Remove Component",
            .description = "Remove a component from every selected object that has it",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasActive,
            .execute = typed<ComponentProperties>([](OperatorContext& context, ComponentProperties& values) {
                Scene& scene = context.editing.scene();
                const ComponentType* type =
                    componentTypesFor(context.editing.document().options()).find(values.component);
                size_t removed = 0;
                for (const SceneEntityId id : context.selection.ids()) {
                    if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                        removed += removeComponent(scene, type, entity, values.component) ? 1 : 0;
                    }
                }
                if (removed == 0) {
                    return OperatorResult::cancelled("No selected object has a '" + values.component + "' component");
                }
                return OperatorResult::finished();
            })};
}

Operator bakeIrradianceVolumeOperator() {
    return {.id = "object.irradiance_volume_bake",
            .label = "Bake Irradiance Volume",
            .description = "Bake the active irradiance volume's lighting into a data file",
            .flags = OperatorFlags::Undo,
            .poll =
                [](const OperatorContext& context) {
                    const Entity active = activeEntity(context);
                    return context.editMode() && active != entt::null &&
                           context.editing.scene().registry().all_of<IrradianceVolumeComponent>(active);
                },
            .execute =
                [](OperatorContext& context, OperatorProperties&) {
                    Scene& scene = context.editing.scene();
                    const EditorDocument& document = context.editing.document();
                    const std::filesystem::path& assetRoot = document.options().assetRoot;
                    const Entity volume = activeEntity(context);
                    auto baked = std::make_shared<IrradianceVolumeData>();
                    std::filesystem::path written;
                    scene.updateWorldTransforms();
                    // Runs on the UI thread; a typical room bakes in well under a second.
                    if (const IrradianceVolumeBakeResult result = bakeIrradianceVolumeFile(
                            scene, context.editing.assets(), volume,
                            irradianceVolumeBakeDirectory(assetRoot, document.path()), {}, *baked, written);
                        !result) {
                        return OperatorResult::failed("Irradiance volume bake failed: " + result.error);
                    }
                    auto& component = scene.registry().get<IrradianceVolumeComponent>(volume);
                    component.bakeAsset = written.lexically_relative(assetRoot).generic_string();
                    component.bakedData = std::move(baked);
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerObjectOperators(OperatorRegistry& registry) {
    std::vector<Operator> operators;
    operators.push_back(addCubeOperator());
    operators.push_back(addObjectOperator("object.add_sun", "Add Sun", "Add a directional sun light", addSun));
    operators.push_back(addObjectOperator("object.add_environment", "Add Environment",
                                          "Add an environment light with a procedural sky", addEnvironment));
    operators.push_back(
        addObjectOperator("object.add_fog", "Add Fog", "Add a volumetric fog medium filling the view", addFog));
    operators.push_back(
        addObjectOperator("object.add_empty", "Add Empty", "Add an entity with only a transform", addEmpty));
    operators.push_back(addEmptyWithOperator());
    operators.push_back(createEntityOperator());
    operators.push_back(duplicateOperator());
    operators.push_back(deleteOperator());
    operators.push_back(selectAllOperator());
    operators.push_back(clearTransformOperator("object.location_clear", "Clear Location", TransformPart::Location));
    operators.push_back(clearTransformOperator("object.rotation_clear", "Clear Rotation", TransformPart::Rotation));
    operators.push_back(clearTransformOperator("object.scale_clear", "Clear Scale", TransformPart::Scale));
    operators.push_back(renameOperator());
    operators.push_back(addComponentOperator());
    operators.push_back(removeComponentOperator());
    operators.push_back(bakeIrradianceVolumeOperator());
    for (Operator& op : operators) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    if (OperatorRegistrationResult result = registerCollectionOperators(registry); !result) {
        return result;
    }
    if (OperatorRegistrationResult result = registerVisibilityOperators(registry); !result) {
        return result;
    }
    return registerOutlinerOperators(registry);
}
