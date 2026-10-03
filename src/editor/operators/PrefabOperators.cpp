#include "PrefabOperators.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"

#include <charconv>
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

PrefabAuthoring authoring(const OperatorContext& context) {
    return PrefabAuthoring(context.editing.assets(), context.editing.document().options());
}

Entity activeEntity(const OperatorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    return active ? context.editing.scene().findEntity(*active) : entt::null;
}

bool hasActive(const OperatorContext& context) {
    return context.editMode() && activeEntity(context) != entt::null;
}

bool activeInInstance(const OperatorContext& context) {
    const Entity active = activeEntity(context);
    return context.editMode() && active != entt::null &&
           prefabInstanceRoot(context.editing.scene(), active) != entt::null;
}

// Reports recoverable problems (orphaned overrides) and turns failures into
// operator results.
OperatorResult finish(OperatorContext& context, const PrefabResult& result) {
    for (const std::string& diagnostic : result.diagnostics) {
        context.editing.diagnostics().add(EditorDiagnosticSeverity::Warning, "Prefabs", diagnostic);
    }
    return result ? OperatorResult::finished() : OperatorResult::failed(result.error);
}

Operator createOperator() {
    return {.id = "prefab.create_from_selection",
            .label = "Create Prefab",
            .description = "Save the active object and its children as a new prefab and link them to it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                CreatePrefabProperties{},
                std::move(Fields<CreatePrefabProperties>().add("path", &CreatePrefabProperties::path))),
            .poll = hasActive,
            .execute = typed<CreatePrefabProperties>([](OperatorContext& context, CreatePrefabProperties& values) {
                if (values.path.empty()) {
                    return OperatorResult::cancelled("Choose where to save the prefab");
                }
                return finish(context,
                              authoring(context).create(context.editing.scene(), activeEntity(context), values.path));
            })};
}

Operator instantiateOperator() {
    return {.id = "prefab.instantiate",
            .label = "Instantiate Prefab",
            .description = "Add an instance of a prefab to the scene and select it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                InstantiatePrefabProperties{},
                std::move(Fields<InstantiatePrefabProperties>().add("asset", &InstantiatePrefabProperties::asset))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute =
                typed<InstantiatePrefabProperties>([](OperatorContext& context, InstantiatePrefabProperties& values) {
                    if (values.asset.empty()) {
                        return OperatorResult::cancelled("Choose a prefab to instantiate");
                    }
                    Scene& scene = context.editing.scene();
                    Entity root = entt::null;
                    const PrefabResult result = authoring(context).instantiate(scene, values.asset, entt::null, root);
                    if (result) {
                        context.selection.selectOnly(scene.id(root));
                    }
                    return finish(context, result);
                })};
}

// Apply, revert, and unpack differ only in the workflow they run.
Operator instanceOperator(std::string id, std::string label, std::string description,
                          PrefabResult (PrefabAuthoring::*run)(Scene&, Entity)) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .flags = OperatorFlags::Undo,
            .poll = activeInInstance,
            .execute = [run](OperatorContext& context, OperatorProperties&) {
                PrefabAuthoring prefabs = authoring(context);
                const PrefabResult result = (prefabs.*run)(context.editing.scene(), activeEntity(context));
                context.selection.removeMissing(context.editing.scene());
                return finish(context, result);
            }};
}

Operator revertFieldOperator() {
    return {.id = "prefab.revert_field",
            .label = "Revert Prefab Value",
            .description = "Reset one overridden value of a prefab instance entity to the prefab's value",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                RevertPrefabFieldProperties{}, std::move(Fields<RevertPrefabFieldProperties>()
                                                             .add("entity", &RevertPrefabFieldProperties::entity)
                                                             .add("component", &RevertPrefabFieldProperties::component)
                                                             .add("field", &RevertPrefabFieldProperties::field))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute = typed<RevertPrefabFieldProperties>([](OperatorContext& context,
                                                             RevertPrefabFieldProperties& values) {
                Scene& scene = context.editing.scene();
                Entity entity = activeEntity(context);
                if (!values.entity.empty()) {
                    uint64_t id = 0;
                    const char* end = values.entity.data() + values.entity.size();
                    if (const auto parsed = std::from_chars(values.entity.data(), end, id);
                        parsed.ec != std::errc{} || parsed.ptr != end) {
                        return OperatorResult::failed("'" + values.entity + "' is not an entity id");
                    }
                    entity = scene.findEntity({id});
                }
                if (entity == entt::null) {
                    return OperatorResult::cancelled("There is no entity to revert");
                }
                return finish(context, authoring(context).revertField(scene, entity, values.component, values.field));
            })};
}
} // namespace

OperatorRegistrationResult registerPrefabOperators(OperatorRegistry& registry) {
    std::vector<Operator> operators;
    operators.push_back(createOperator());
    operators.push_back(instantiateOperator());
    operators.push_back(instanceOperator("prefab.apply", "Apply Prefab",
                                         "Write the instance's changes into its prefab and update its other instances",
                                         &PrefabAuthoring::apply));
    operators.push_back(instanceOperator("prefab.revert", "Revert Prefab",
                                         "Reset the instance to its prefab, dropping overrides and added objects",
                                         &PrefabAuthoring::revert));
    operators.push_back(instanceOperator("prefab.unpack", "Unpack Prefab",
                                         "Keep the instance's objects but remove their link to the prefab",
                                         &PrefabAuthoring::unpack));
    operators.push_back(revertFieldOperator());
    for (Operator& op : operators) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
