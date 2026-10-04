#include "ComponentOperators.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <utility>

namespace {
using Json = nlohmann::json;
using ExecuteFunction = std::function<OperatorResult(OperatorContext&, OperatorProperties&)>;

constexpr const char* clipboardMarker = "zenith_component";

template <typename Properties, typename Function> ExecuteFunction typed(Function run) {
    return [run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(context, *values);
    };
}

Entity activeEntity(const OperatorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    return active ? context.editing.scene().findEntity(*active) : entt::null;
}

// In Play and Simulate the operators edit the runtime scene live, unrecorded.
bool hasActive(const OperatorContext& context) {
    return activeEntity(context) != entt::null;
}

bool hasSeveralSelected(const OperatorContext& context) {
    return hasActive(context) && context.selection.size() > 1;
}

// The selected entities that exist, active first.
std::vector<Entity> selectedEntities(const OperatorContext& context) {
    std::vector<Entity> entities;
    for (const SceneEntityId id : context.selection.ids()) {
        if (const Entity entity = context.editing.scene().findEntity(id); entity != entt::null) {
            entities.push_back(entity);
        }
    }
    return entities;
}

// The user-visible component type named by key, or null.
const ComponentType* findUserType(const OperatorContext& context, std::string_view key) {
    const ComponentType* type = componentTypesFor(context.editing.document().options()).find(key);
    return type != nullptr && !type->internal() ? type : nullptr;
}

OperatorResult unknownType(std::string_view key) {
    return OperatorResult::failed("Unknown component type '" + std::string(key) + "'");
}

ComponentSaveContext saveContext(const OperatorContext& context) {
    return {.assets = context.editing.assets(), .options = context.editing.document().options()};
}

ComponentLoadContext loadContext(const OperatorContext& context) {
    return {.assets = context.editing.assets(), .options = context.editing.document().options()};
}

// Loads value into every target; a value that loads into the first loads into
// all, so a failure leaves the scene unchanged.
OperatorResult loadInto(OperatorContext& context, const ComponentType& type, const Json& value,
                        const std::vector<Entity>& targets) {
    for (const Entity target : targets) {
        if (const SceneDocumentResult result = type.load(value, context.editing.scene(), target, loadContext(context));
            !result) {
            return OperatorResult::failed("Could not paste " + type.displayName() + " values: " + result.error);
        }
    }
    return OperatorResult::finished();
}

Fields<ComponentProperties> componentFields() {
    return std::move(Fields<ComponentProperties>().add("component", &ComponentProperties::component));
}

Operator resetOperator() {
    return {.id = "object.component_reset",
            .label = "Reset Component",
            .description = "Reset a component to its default values on every selected object that has it",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasActive,
            .execute = typed<ComponentProperties>([](OperatorContext& context, ComponentProperties& values) {
                const ComponentType* type = findUserType(context, values.component);
                if (type == nullptr) {
                    return unknownType(values.component);
                }
                size_t reset = 0;
                for (const Entity entity : selectedEntities(context)) {
                    reset += type->resetToDefault(context.editing.scene(), entity) ? 1 : 0;
                }
                if (reset == 0) {
                    return OperatorResult::cancelled("No selected object has a " + type->displayName() + " component");
                }
                return OperatorResult::finished();
            })};
}

Operator copyOperator(TextClipboard& clipboard) {
    return {.id = "object.component_copy",
            .label = "Copy Component Values",
            .description = "Copy the active object's component values to the clipboard",
            .flags = OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasActive,
            .execute = typed<ComponentProperties>([&clipboard](OperatorContext& context, ComponentProperties& values) {
                const ComponentType* type = findUserType(context, values.component);
                if (type == nullptr) {
                    return unknownType(values.component);
                }
                const Entity active = activeEntity(context);
                if (!type->has(context.editing.scene(), active)) {
                    return OperatorResult::cancelled("The active object has no " + type->displayName() + " component");
                }
                Json value = Json::object();
                if (const SceneDocumentResult result =
                        type->save(context.editing.scene(), active, saveContext(context), value);
                    !result) {
                    return OperatorResult::failed("Could not copy " + type->displayName() + ": " + result.error);
                }
                clipboard.setText(componentClipboardText(type->key(), value));
                return OperatorResult::finished();
            })};
}

// The component key and value held by clipboard text, or nullopt when it
// holds no component values.
std::optional<std::pair<std::string, Json>> parseClipboard(const std::string& text) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    const auto key = document.find(clipboardMarker);
    const auto value = document.find("value");
    if (key == document.end() || !key->is_string() || value == document.end() || !value->is_object()) {
        return std::nullopt;
    }
    return std::pair{key->get<std::string>(), *value};
}

Operator pasteOperator(TextClipboard& clipboard) {
    return {.id = "object.component_paste",
            .label = "Paste Component Values",
            .description = "Paste copied component values onto every selected object that has the component",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasActive,
            .execute = typed<ComponentProperties>([&clipboard](OperatorContext& context, ComponentProperties& values) {
                const std::optional<std::pair<std::string, Json>> copied = parseClipboard(clipboard.text());
                if (!copied) {
                    return OperatorResult::cancelled("The clipboard holds no component values");
                }
                // An empty component property pastes whatever type was copied.
                const std::string& key = values.component.empty() ? copied->first : values.component;
                const ComponentType* type = findUserType(context, key);
                if (type == nullptr) {
                    return unknownType(key);
                }
                if (copied->first != key) {
                    return OperatorResult::cancelled("The clipboard holds " + copied->first + " values, not " +
                                                     type->displayName());
                }
                std::vector<Entity> targets;
                for (const Entity entity : selectedEntities(context)) {
                    if (type->has(context.editing.scene(), entity)) {
                        targets.push_back(entity);
                    }
                }
                if (targets.empty()) {
                    return OperatorResult::cancelled("No selected object has a " + type->displayName() + " component");
                }
                return loadInto(context, *type, copied->second, targets);
            })};
}

Operator copyToSelectedOperator() {
    return {.id = "object.component_copy_to_selected",
            .label = "Copy Component to Selected",
            .description = "Copy the active object's component, with its values, to the other selected objects",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(ComponentProperties{}, componentFields()),
            .poll = hasSeveralSelected,
            .execute = typed<ComponentProperties>([](OperatorContext& context, ComponentProperties& values) {
                const ComponentType* type = findUserType(context, values.component);
                if (type == nullptr) {
                    return unknownType(values.component);
                }
                std::vector<Entity> targets = selectedEntities(context);
                const Entity active = targets.front();
                Json value = Json::object();
                if (!type->has(context.editing.scene(), active)) {
                    return OperatorResult::cancelled("The active object has no " + type->displayName() + " component");
                }
                if (const SceneDocumentResult result =
                        type->save(context.editing.scene(), active, saveContext(context), value);
                    !result) {
                    return OperatorResult::failed("Could not copy " + type->displayName() + ": " + result.error);
                }
                targets.erase(targets.begin());
                return loadInto(context, *type, value, targets);
            })};
}

Operator moveOperator(ComponentInspectorLayout& layout) {
    return {.id = "object.component_move",
            .label = "Move Component",
            .description = "Move a component's section up or down in the inspector for every object",
            .flags = OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                ComponentMoveProperties{},
                std::move(Fields<ComponentMoveProperties>()
                              .add("component", &ComponentMoveProperties::component)
                              .add("category", &ComponentMoveProperties::category)
                              .add("direction", &ComponentMoveProperties::direction)
                              .options({{"up", "Up", static_cast<int64_t>(ComponentMoveDirection::Up)},
                                        {"down", "Down", static_cast<int64_t>(ComponentMoveDirection::Down)},
                                        {"top", "Top", static_cast<int64_t>(ComponentMoveDirection::Top)},
                                        {"bottom", "Bottom", static_cast<int64_t>(ComponentMoveDirection::Bottom)}}))),
            .poll = hasActive,
            .execute =
                typed<ComponentMoveProperties>([&layout](OperatorContext& context, ComponentMoveProperties& values) {
                    const Scene& scene = context.editing.scene();
                    const std::vector<std::string> displayed =
                        displayedComponentKeys(componentTypesFor(context.editing.document().options()), layout, scene,
                                               *context.selection.active(), values.category);
                    if (!layout.move(values.component, values.direction, displayed)) {
                        return OperatorResult::cancelled("The component cannot move that way");
                    }
                    return OperatorResult::finished();
                })};
}
} // namespace

std::string componentClipboardText(std::string_view componentKey, const Json& value) {
    return Json{{clipboardMarker, componentKey}, {"value", value}}.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::vector<std::string> displayedComponentKeys(const ComponentTypeRegistry& types,
                                                const ComponentInspectorLayout& layout, const Scene& scene,
                                                SceneEntityId entity, std::string_view category) {
    std::vector<std::string> keys;
    const Entity found = scene.findEntity(entity);
    if (found == entt::null) {
        return keys;
    }
    for (const ComponentType& type : types.types()) {
        if (!type.internal() && type.has(scene, found) &&
            (category.empty() || componentCategoryLabel(type) == category)) {
            keys.push_back(type.key());
        }
    }
    return layout.sorted(std::move(keys));
}

OperatorRegistrationResult registerComponentOperators(OperatorRegistry& registry, ComponentOperatorServices services) {
    std::vector<Operator> operators;
    operators.push_back(resetOperator());
    operators.push_back(copyOperator(services.clipboard));
    operators.push_back(pasteOperator(services.clipboard));
    operators.push_back(copyToSelectedOperator());
    operators.push_back(moveOperator(services.layout));
    for (Operator& op : operators) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerComponentOperatorFamily(OperatorRegistry& registry,
                                                           const EditorOperatorServices& services) {
    return registerComponentOperators(registry,
                                      {.clipboard = services.componentClipboard, .layout = services.componentLayout});
}
