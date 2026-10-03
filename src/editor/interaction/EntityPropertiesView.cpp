#include "EntityPropertiesView.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <any>
#include <array>
#include <utility>

namespace {
using Json = nlohmann::json;

const ImVec4 problemColor = toImVec4(palette::error);

// Properties for the operators taking a component type key.
std::string componentProperties(std::string_view key) {
    return Json{{"component", key}}.dump();
}

template <typename Component> EntityPropertiesView::CustomEditor typedEditor(bool (*draw)(Component&)) {
    return [draw](EditorContext&, Entity, void* component) { return draw(*static_cast<Component*>(component)); };
}

const ComponentTypeRegistry& componentTypes(const EditorContext& context) {
    return componentTypesFor(context.editing.document().options());
}

ComponentSaveContext saveContext(const EditorContext& context) {
    return {.assets = context.app.assets, .options = context.editing.document().options()};
}

// The component's document form, or nullopt (after reporting) when it cannot
// be written.
std::optional<Json> componentJson(const EditorContext& context, const ComponentType& type, Entity entity) {
    Json value = Json::object();
    if (const SceneDocumentResult result = type.save(context.app.scene, entity, saveContext(context), value); !result) {
        context.diagnostics.reportError("Could not read " + type.displayName() + ": " + result.error);
        return std::nullopt;
    }
    return value;
}

// Copies a custom editor's edit of targets.front() (from original) to the
// other targets through the component's document form: only the top-level
// keys the edit changed are written, so the targets' other differences
// survive.
void copyCustomEdit(EditorContext& context, const ComponentType& type, std::span<const Entity> targets,
                    const std::any& original) {
    Scene& scene = context.app.scene;
    void* edited = type.find(scene, targets.front());
    const std::any editedValue = type.copy(edited);
    const std::optional<Json> after = componentJson(context, type, targets.front());
    type.assign(edited, original);
    const std::optional<Json> before = componentJson(context, type, targets.front());
    type.assign(edited, editedValue);
    if (!after || !before) {
        return;
    }
    const ComponentLoadContext load{.assets = context.editing.assets(),
                                    .options = context.editing.document().options()};
    for (const Entity target : targets.subspan(1)) {
        std::optional<Json> value = componentJson(context, type, target);
        if (!value) {
            continue;
        }
        for (const auto& [key, changed] : after->items()) {
            if (const auto previous = before->find(key); previous == before->end() || *previous != changed) {
                (*value)[key] = changed;
            }
        }
        if (const SceneDocumentResult result = type.load(*value, scene, target, load); !result) {
            context.diagnostics.reportError("Could not apply the " + type.displayName() + " edit: " + result.error);
        }
    }
}

// Whether any target's component differs from the first's.
bool customValuesDiffer(const EditorContext& context, const ComponentType& type, std::span<const Entity> targets) {
    if (targets.size() < 2) {
        return false;
    }
    const std::optional<Json> shown = componentJson(context, type, targets.front());
    return std::ranges::any_of(targets.subspan(1),
                               [&](Entity target) { return componentJson(context, type, target) != shown; });
}

// "component:<id>,<id>:<key>", so a drag over the same entities merges.
std::string editCoalesceKey(const Scene& scene, std::span<const Entity> targets, const ComponentType& type) {
    std::string key = "component:";
    for (size_t index = 0; index < targets.size(); ++index) {
        key += (index == 0 ? "" : ",") + std::to_string(scene.id(targets[index]).value);
    }
    return key + ':' + type.key();
}
} // namespace

std::string componentCategoryLabel(const ComponentType& type) {
    return type.category().empty() ? std::string("Other") : type.category();
}

std::vector<std::string> entityComponentCategories(const ComponentTypeRegistry& types, const Scene& scene,
                                                   SceneEntityId entityId) {
    std::vector<std::string> categories;
    const Entity entity = scene.findEntity(entityId);
    if (entity == entt::null) {
        return categories;
    }
    for (const ComponentType& type : types.types()) {
        if (type.internal() || !type.has(scene, entity)) {
            continue;
        }
        std::string label = componentCategoryLabel(type);
        if (std::ranges::find(categories, label) == categories.end()) {
            categories.push_back(std::move(label));
        }
    }
    return categories;
}

EntityPropertiesView::EntityPropertiesView(ComponentInspectorLayout& layout)
    : m_layout(layout) {
    m_customEditors.emplace("collider", typedEditor(ComponentEditors::drawCollider));
    m_customEditors.emplace("morph_weights", typedEditor(ComponentEditors::drawMorphWeights));
    m_customEditors.emplace("irradiance_volume", typedEditor(ComponentEditors::drawIrradianceVolume));
    m_customEditors.emplace("animation", [](EditorContext& context, Entity entity, void* component) {
        // Clip names and durations come from the entity's model.
        const auto* model = context.app.scene.registry().try_get<ModelComponent>(entity);
        const ModelAsset* asset = model != nullptr ? context.app.assets.model(model->model) : nullptr;
        return ComponentEditors::drawAnimation(*static_cast<AnimationComponent*>(component), asset);
    });
}

void EntityPropertiesView::drawMultipleSelection(EditorContext& context) const {
    ImGui::Text("%zu entities selected", context.selection.size());
    if (widgets::Button("Delete All")) {
        context.invoke("object.delete");
    }
    ImGui::TextDisabled("Edits apply to every selected entity; %s marks values that differ.", mixedValueText);
}

void EntityPropertiesView::drawName(EditorContext& context, SceneEntityId entityId) const {
    const Entity entity = context.app.scene.findEntity(entityId);
    if (entity == entt::null) {
        return;
    }
    std::string& name = context.app.scene.registry().get<NameComponent>(entity).name;
    std::array<char, 256> nameBuffer{};
    const size_t nameLength = std::min(name.size(), nameBuffer.size() - 1);
    std::copy_n(name.data(), nameLength, nameBuffer.data());
    if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size())) {
        std::string beforeName;
        if (context.editing.capture(beforeName)) {
            name = nameBuffer.data();
            context.editing.commit("Rename Entity", "name:" + std::to_string(entityId.value), std::move(beforeName));
        }
    }
    ImGui::TextDisabled("ID %llu", static_cast<unsigned long long>(entityId.value));
}

void EntityPropertiesView::drawFilter(const char* id, std::string& filter) const {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(id, "Filter properties...", &filter);
}

void EntityPropertiesView::drawTransform(EditorContext& context, SceneEntityId entityId,
                                         std::string_view filter) const {
    if (!widgets::SectionHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) ||
        !matchesInspectorFilter("Transform", filter)) {
        return;
    }
    Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(entityId);
    if (entity == entt::null) {
        return;
    }
    TransformComponent& transform = scene.registry().get<TransformComponent>(entity);
    const TransformComponent original = transform;
    if (ComponentEditors::drawTransform(transform)) {
        // The widget edits in place; the snapshot needs the unedited value.
        const TransformComponent edited = transform;
        transform = original;
        std::string before;
        if (context.editing.capture(before)) {
            transform = edited;
            context.editing.commit("Edit Transform", "transform:" + std::to_string(entityId.value), std::move(before));
        }
    }
}

void EntityPropertiesView::drawComponents(EditorContext& context, std::span<const SceneEntityId> entityIds,
                                          std::string_view filter, std::optional<std::string_view> category) const {
    const Scene& scene = context.app.scene;
    std::vector<SceneEntityId> entities;
    for (const SceneEntityId id : entityIds) {
        if (scene.findEntity(id) != entt::null) {
            entities.push_back(id);
        }
    }
    if (entities.empty()) {
        return;
    }
    const ComponentTypeRegistry& types = componentTypes(context);
    std::vector<const ComponentType*> shared;
    std::vector<const ComponentType*> activeOnly;
    const std::string_view listedCategory = category.value_or(std::string_view{});
    for (const std::string& key : displayedComponentKeys(types, m_layout, scene, entities.front(), listedCategory)) {
        const ComponentType* type = types.find(key);
        const bool everyEntity =
            std::ranges::all_of(entities, [&](SceneEntityId id) { return type->has(scene, scene.findEntity(id)); });
        (everyEntity ? shared : activeOnly).push_back(type);
    }
    for (const ComponentType* type : shared) {
        drawComponent(context, *type, entities, {.filter = filter, .category = listedCategory});
    }
    if (activeOnly.empty()) {
        return;
    }
    ImGui::SeparatorText("Active Entity Only");
    ImGui::TextDisabled("Hold Alt while editing to apply a change to every selected entity with the component.");
    for (const ComponentType* type : activeOnly) {
        drawComponent(context, *type, std::span(entities).first(1),
                      {.filter = filter, .actions = HeaderActions::ActiveEntity, .category = listedCategory});
    }
}

void EntityPropertiesView::drawComponent(EditorContext& context, const ComponentType& type,
                                         std::span<const SceneEntityId> entityIds,
                                         const ComponentDrawOptions& options) const {
    const Scene& scene = context.app.scene;
    std::vector<Entity> shown;
    for (const SceneEntityId id : entityIds) {
        if (const Entity entity = scene.findEntity(id); entity != entt::null && type.has(scene, entity)) {
            shown.push_back(entity);
        }
    }
    if (shown.empty()) {
        return;
    }
    const std::string_view filter = options.filter;
    const bool nameMatches = matchesInspectorFilter(type.displayName(), filter);
    if (!nameMatches && !m_customEditors.contains(type.key()) &&
        std::ranges::none_of(type.fields(), [filter](const FieldDescriptor& field) {
            return matchesInspectorFilter(field.presentation.label, filter);
        })) {
        return;
    }

    ImGui::PushID(type.key().c_str());
    // Sections open and close per component type, remembered by the layout.
    // ImGui's text log must not force them open, or it would expand them.
    ImGui::SetNextItemOpen(!m_layout.collapsed(type.key()), ImGuiCond_Always);
    const bool open = widgets::SectionHeader(type.displayName().c_str(), ImGuiTreeNodeFlags_NoAutoOpenOnLog);
    m_layout.setCollapsed(type.key(), !open);
    std::optional<HeaderAction> action;
    if (options.actions != HeaderActions::None) {
        action = drawComponentMenu(context, type, options);
    }
    if (open && !action) {
        drawComponentValues(context, type, shown, nameMatches ? std::string_view{} : filter);
    }
    if (action) {
        context.invoke(action->operatorId, action->properties);
    }
    ImGui::PopID();
}

std::optional<EntityPropertiesView::HeaderAction>
EntityPropertiesView::drawComponentMenu(EditorContext& context, const ComponentType& type,
                                        const ComponentDrawOptions& options) const {
    const std::string properties = componentProperties(type.key());
    const auto run = [&properties](const char* operatorId) {
        return std::optional<HeaderAction>(HeaderAction{.operatorId = operatorId, .properties = properties});
    };
    const auto move = [&type, &options](const char* direction) {
        return std::optional<HeaderAction>(HeaderAction{
            .operatorId = "object.component_move",
            .properties =
                Json{{"component", type.key()}, {"direction", direction}, {"category", options.category}}.dump()});
    };
    const bool wholeSelection = options.actions == HeaderActions::Selection;
    // Right-clicking the header or pressing "..." opens the same menu.
    std::optional<HeaderAction> action;
    if (ImGui::BeginPopupContextItem("##ComponentMenu")) {
        if (wholeSelection && ImGui::MenuItem("Remove")) {
            action = run("object.component_remove");
        }
        if (wholeSelection && ImGui::MenuItem("Reset to Default")) {
            action = run("object.component_reset");
        }
        if (wholeSelection) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Copy Values")) {
            action = run("object.component_copy");
        }
        if (wholeSelection && ImGui::MenuItem("Paste Values")) {
            action = run("object.component_paste");
        }
        if (ImGui::MenuItem("Copy to Selected", nullptr, false, context.selection.size() > 1)) {
            action = run("object.component_copy_to_selected");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move Up")) {
            action = move("up");
        }
        if (ImGui::MenuItem("Move Down")) {
            action = move("down");
        }
        if (ImGui::MenuItem("Move to Top")) {
            action = move("top");
        }
        if (ImGui::MenuItem("Move to Bottom")) {
            action = move("bottom");
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) {
        ImGui::OpenPopup("##ComponentMenu");
    }
    if (wholeSelection) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            action = run("object.component_remove");
        }
    }
    return action;
}

void EntityPropertiesView::drawComponentValues(EditorContext& context, const ComponentType& type,
                                               std::span<const Entity> shown, std::string_view filter) const {
    Scene& scene = context.app.scene;
    // Alt applies the edit to every selected entity with the component, not
    // only to those shown; their values are not compared. A component of an
    // entity outside the selection (the World tab) is edited alone.
    std::vector<Entity> targets(shown.begin(), shown.end());
    if (ImGui::GetIO().KeyAlt && context.selection.contains(scene.id(shown.front()))) {
        for (const SceneEntityId id : context.selection.ids()) {
            const Entity entity = scene.findEntity(id);
            if (entity != entt::null && type.has(scene, entity) &&
                std::ranges::find(targets, entity) == targets.end()) {
                targets.push_back(entity);
            }
        }
    }
    std::vector<void*> components;
    // Widgets edit in place; undo needs snapshots of the unedited values.
    std::vector<std::any> originals;
    for (const Entity target : targets) {
        components.push_back(type.find(scene, target));
        originals.push_back(type.copy(components.back()));
    }
    const auto custom = m_customEditors.find(type.key());
    bool changed = false;
    if (custom != m_customEditors.end()) {
        if (customValuesDiffer(context, type, shown)) {
            ImGui::TextDisabled("Values differ (%s); an edit applies only the changed setting.", mixedValueText);
        }
        changed = custom->second(context, targets.front(), components.front());
        if (changed && targets.size() > 1) {
            copyCustomEdit(context, type, targets, originals.front());
        }
    } else {
        // The eyedropper fills the active entity's field; the multi-edit copy
        // below carries the change to the other targets.
        const EntityReferenceFieldContext entityReferences =
            entityReferenceFieldContext(context, scene.id(targets.front()), type);
        const ReflectedFieldEditorOptions fieldOptions{
            .filter = filter,
            .assetLoaded =
                [&type, &scene, entity = targets.front()](std::string_view fieldKey) {
                    return type.textureLoaded(type.find(scene, entity), fieldKey);
                },
            .entityReferences = &entityReferences,
            .showMixedValues = targets.size() == shown.size()};
        changed = drawReflectedFields(components, type.fields(), fieldOptions);
    }
    if (changed) {
        // Copying a custom edit may have replaced the components, so look
        // each one up again.
        std::vector<std::any> edited;
        for (size_t index = 0; index < targets.size(); ++index) {
            void* component = type.find(scene, targets[index]);
            type.sanitize(component);
            edited.push_back(type.copy(component));
            type.assign(component, originals[index]);
        }
        std::string before;
        if (context.editing.capture(before)) {
            for (size_t index = 0; index < targets.size(); ++index) {
                type.assign(type.find(scene, targets[index]), edited[index]);
            }
            context.editing.commit("Edit " + type.displayName(), editCoalesceKey(scene, targets, type),
                                   std::move(before));
        }
    }
    if (const std::string problem = type.validate(type.find(scene, targets.front())); !problem.empty()) {
        ImGui::TextColored(problemColor, "%s", problem.c_str());
    }
}

void EntityPropertiesView::drawPreservedComponents(EditorContext& context, SceneEntityId entityId) const {
    Scene& scene = context.app.scene;
    const auto* preserved = scene.registry().try_get<PreservedComponentsComponent>(scene.findEntity(entityId));
    if (preserved == nullptr) {
        return;
    }
    // Copied because removing an entry edits the component being iterated.
    const std::vector<std::pair<std::string, std::string>> entries = preserved->components;
    for (const auto& [key, payload] : entries) {
        ImGui::PushID(key.c_str());
        const std::string header = key + " (unknown type)";
        const bool open = widgets::SectionHeader(header.c_str());
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        if (open && !remove) {
            ImGui::TextWrapped("No registered component type uses this key, for example because the game module "
                               "that defines it is not loaded. Its data is kept unchanged and saved with the scene.");
            ImGui::TextDisabled("%s", payload.c_str());
        }
        if (remove) {
            context.invoke("object.component_remove", componentProperties(key));
        }
        ImGui::PopID();
        if (remove) {
            break;
        }
    }
}

void EntityPropertiesView::drawIrradianceBakeButton(EditorContext& context, SceneEntityId entityId) const {
    Scene& scene = context.app.scene;
    if (scene.registry().all_of<IrradianceVolumeComponent>(scene.findEntity(entityId)) &&
        widgets::Button("Bake Irradiance Volume", {-1.0f, 0.0f})) {
        context.invoke("object.irradiance_volume_bake");
    }
}

void EntityPropertiesView::drawAssetWarnings(EditorContext& context, SceneEntityId entityId) const {
    const entt::registry& registry = context.app.scene.registry();
    const Entity entity = context.app.scene.findEntity(entityId);
    if (const auto* model = registry.try_get<ModelComponent>(entity);
        model != nullptr && context.app.assets.model(model->model) == nullptr) {
        ImGui::TextColored(problemColor, "Model asset reference is invalid or missing.");
    }
    if (const auto* mesh = registry.try_get<MeshRendererComponent>(entity);
        mesh != nullptr && context.app.assets.mesh(mesh->mesh) == nullptr) {
        ImGui::TextColored(problemColor, "Mesh asset reference is invalid or missing.");
    }
}

void EntityPropertiesView::drawModelDropTarget(EditorContext& context, SceneEntityId entityId) const {
    widgets::Button("Drop Model Asset to Add Child", {-1.0f, 0.0f});
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            context.placeModel(static_cast<const char*>(payload->Data), context.app.scene.findEntity(entityId));
        }
        ImGui::EndDragDropTarget();
    }
}

void EntityPropertiesView::drawAddComponentMenu(EditorContext& context, SceneEntityId entityId,
                                                std::optional<std::string_view> category) const {
    if (widgets::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    if (!ImGui::BeginPopup("AddComponentPopup")) {
        return;
    }
    Scene& scene = context.app.scene;
    const ComponentTypeRegistry& types = componentTypes(context);
    // Grouped by category in the order categories first appear.
    std::vector<std::string> categories;
    for (const ComponentType& type : types.types()) {
        std::string label = componentCategoryLabel(type);
        if (!type.internal() && (!category || label == *category) &&
            std::ranges::find(categories, label) == categories.end()) {
            categories.push_back(std::move(label));
        }
    }
    for (const std::string& label : categories) {
        if (!category) {
            ImGui::SeparatorText(label.c_str());
        }
        for (const ComponentType& type : types.types()) {
            if (type.internal() || componentCategoryLabel(type) != label ||
                type.has(scene, scene.findEntity(entityId))) {
                continue;
            }
            if (ImGui::MenuItem(type.displayName().c_str())) {
                context.invoke("object.component_add", componentProperties(type.key()));
            }
        }
    }
    ImGui::EndPopup();
}
