#include "EntityPropertiesView.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/interaction/AssetReferenceField.hpp"
#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/interaction/ProjectNames.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <any>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

// Properties of prefab.revert_field for one value of entity.
std::string revertFieldProperties(SceneEntityId entity, std::string_view component, std::string_view field) {
    return Json{{"entity", std::to_string(entity.value)}, {"component", component}, {"field", field}}.dump();
}

// "component:<id>,<id>:<key>", so a drag over the same entities merges.
std::string editCoalesceKey(const Scene& scene, std::span<const Entity> targets, const ComponentType& type) {
    std::string key = "component:";
    for (size_t index = 0; index < targets.size(); ++index) {
        key += (index == 0 ? "" : ",") + std::to_string(scene.id(targets[index]).value);
    }
    return key + ':' + type.key();
}

// prefix followed by the entities' ids, "name:<id>,<id>", so typing or a drag
// over the same entities merges into one step.
std::string entitiesCoalesceKey(std::string_view prefix, const Scene& scene, std::span<const Entity> entities) {
    std::string key(prefix);
    for (size_t index = 0; index < entities.size(); ++index) {
        key += (index == 0 ? "" : ",") + std::to_string(scene.id(entities[index]).value);
    }
    return key;
}

// Sets each axis of transform that is within decomposition noise of shown's
// to shown's value.
void alignTransformNoise(const EditableTransform& shown, EditableTransform& transform) {
    constexpr float tolerance = 1.0e-4f;
    const auto align = [](const glm::vec3& reference, glm::vec3& value) {
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            if (std::abs(value[axis] - reference[axis]) <= tolerance) {
                value[axis] = reference[axis];
            }
        }
    };
    align(shown.translation, transform.translation);
    align(shown.rotationDegrees, transform.rotationDegrees);
    align(shown.scale, transform.scale);
}

// exact with the axes that changed from shown to edited replaced by their
// edited values.
EditableTransform editedAxes(const EditableTransform& exact, const EditableTransform& shown,
                             const EditableTransform& edited) {
    EditableTransform result = exact;
    const auto apply = [](const glm::vec3& before, const glm::vec3& after, glm::vec3& value) {
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            if (after[axis] != before[axis]) {
                value[axis] = after[axis];
            }
        }
    };
    apply(shown.translation, edited.translation, result.translation);
    apply(shown.rotationDegrees, edited.rotationDegrees, result.rotationDegrees);
    apply(shown.scale, edited.scale, result.scale);
    return result;
}

// The entities of ids that exist, in order.
std::vector<Entity> existingEntities(const Scene& scene, std::span<const SceneEntityId> ids) {
    std::vector<Entity> entities;
    for (const SceneEntityId id : ids) {
        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
            entities.push_back(entity);
        }
    }
    return entities;
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
    // The tag being typed; shared by copies of the editor.
    m_customEditors.emplace("tags", [otherTag = std::make_shared<std::string>()](EditorContext& context, Entity,
                                                                                 void* component) {
        const std::span<const std::string> projectTags = context.projectNames != nullptr
                                                             ? std::span<const std::string>(context.projectNames->tags)
                                                             : std::span<const std::string>{};
        return ComponentEditors::drawTags(*static_cast<TagsComponent*>(component), projectTags, *otherTag);
    });
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
    drawName(context, std::span(&entityId, 1));
}

void EntityPropertiesView::drawName(EditorContext& context, std::span<const SceneEntityId> entityIds) const {
    Scene& scene = context.app.scene;
    const std::vector<Entity> entities = existingEntities(scene, entityIds);
    if (entities.empty()) {
        return;
    }
    entt::registry& registry = scene.registry();
    const std::string& shownName = registry.get<NameComponent>(entities.front()).name;
    const bool mixed = std::ranges::any_of(std::span(entities).subspan(1), [&](Entity entity) {
        return registry.get<NameComponent>(entity).name != shownName;
    });
    // Differing names show the mixed mark; typing names every entity alike.
    std::string typed = mixed ? std::string() : shownName;
    const bool edited =
        mixed ? ImGui::InputTextWithHint("Name", mixedValueText, &typed) : ImGui::InputText("Name", &typed);
    if (edited) {
        SceneEditing::EntitySnapshot before;
        if (context.editing.captureEntities(entities, before)) {
            for (const Entity entity : entities) {
                registry.get<NameComponent>(entity).name = typed;
            }
            context.editing.commit(entities.size() == 1 ? "Rename Entity" : "Rename Entities",
                                   entitiesCoalesceKey("name:", scene, entities), std::move(before));
        }
    }
    if (entities.size() != 1) {
        return;
    }
    const SceneEntityId entityId = scene.id(entities.front());
    bool revert = false;
    if (prefabOverrides(context, entityId).overridden("", "/name")) {
        markPrefabOverride("Name");
        revert = prefabOverrideMenu("##NameOverride");
    }
    ImGui::TextDisabled("ID %llu", static_cast<unsigned long long>(entityId.value));
    if (revert) {
        context.invoke("prefab.revert_field", revertFieldProperties(entityId, "", "/name"));
    }
}

void EntityPropertiesView::drawRuntimeNotice(EditorContext& context) const {
    if (context.editMode()) {
        return;
    }
    ImGui::PushID("##RuntimeNotice");
    widgets::CaptionText("Live: editing the running scene", palette::warning);
    ImGui::TextWrapped("Edits apply at once, have no undo, and are lost on Stop. Apply to Edit Scene (here or on "
                       "a component's menu) keeps them as one undoable edit.");
    ImGui::BeginDisabled(!context.canInvoke("play.apply_to_edit") || context.selection.empty());
    if (widgets::Button("Apply Selection to Edit Scene")) {
        context.invoke("play.apply_to_edit");
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::PopID();
}

const PrefabEntityOverrides& EntityPropertiesView::prefabOverrides(const EditorContext& context,
                                                                   SceneEntityId entityId) const {
    if (!context.editMode()) {
        // Prefab overrides describe the document; the running scene has none
        // to show or revert.
        static const PrefabEntityOverrides none;
        return none;
    }
    const PrefabAuthoring authoring(context.editing.assets(), context.editing.document().options());
    return m_prefabOverrides.get(authoring, context.app.scene, entityId, context.editing.revision());
}

void EntityPropertiesView::drawPrefabHeader(EditorContext& context, SceneEntityId entityId) const {
    // Copied: running an operator below changes the revision the cache keys on.
    const PrefabEntityOverrides overrides = prefabOverrides(context, entityId);
    if (!overrides.linked) {
        return;
    }
    // In prefab mode the document root's instance is the prefab itself:
    // Apply would be Save, and Revert would discard the document's edits.
    if (context.prefabEdit != nullptr && overrides.instanceRoot == context.prefabEdit->root) {
        return;
    }
    ImGui::PushID("##PrefabHeader");
    ImGui::SeparatorText("Prefab");
    widgets::CaptionText(overrides.displayName.c_str(), palette::prefabOverrideInk);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", overrides.prefab.c_str());
    }
    if (!m_prefabOverrides.error().empty()) {
        ImGui::TextColored(problemColor, "%s", m_prefabOverrides.error().c_str());
    } else if (overrides.added) {
        ImGui::TextDisabled("Added to the instance in this scene");
    } else if (overrides.instanceOverrides == 0) {
        ImGui::TextDisabled("Matches its prefab");
    } else {
        ImGui::TextDisabled("%zu override%s on the instance", overrides.instanceOverrides,
                            overrides.instanceOverrides == 1 ? "" : "s");
        if (overrides.removedEntities > 0) {
            ImGui::TextDisabled("%zu prefab object%s deleted (Revert restores)", overrides.removedEntities,
                                overrides.removedEntities == 1 ? "" : "s");
        }
    }
    std::optional<std::pair<const char*, std::string>> action;
    if (overrides.parent) {
        ImGui::TextUnformatted("Moved under another parent");
        markPrefabOverride({});
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert Parent")) {
            action.emplace("prefab.revert_field", revertFieldProperties(entityId, "", "/parent"));
        }
    }
    // Apply, Revert, and Unpack act on the active entity's instance.
    const bool active = context.selection.active() == entityId;
    ImGui::BeginDisabled(!active);
    for (const auto& [label, operatorId] : {std::pair{"Apply", "prefab.apply"}, std::pair{"Revert", "prefab.revert"},
                                            std::pair{"Unpack", "prefab.unpack"}}) {
        if (widgets::Button(label)) {
            action.emplace(operatorId, std::string{});
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!context.canInvoke("prefab.select_asset"));
    if (widgets::Button("Select Prefab Asset")) {
        action.emplace("prefab.select_asset", std::string{});
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // Edits the prefab this header shows, so any member of the instance may
    // start it.
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.canInvoke("prefab.edit"));
    if (widgets::Button("Edit Prefab")) {
        action.emplace("prefab.edit", nlohmann::json{{"asset", overrides.prefab}}.dump());
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Open the prefab as a document of its own");
    ImGui::PopID();
    if (action) {
        context.invoke(action->first, action->second);
    }
}

void EntityPropertiesView::drawFilter(const char* id, std::string& filter) const {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(id, "Filter properties...", &filter);
}

void EntityPropertiesView::drawTransform(EditorContext& context, SceneEntityId entityId,
                                         std::string_view filter) const {
    drawTransform(context, std::span(&entityId, 1), filter);
}

void EntityPropertiesView::drawTransform(EditorContext& context, std::span<const SceneEntityId> entityIds,
                                         std::string_view filter) const {
    const bool open = widgets::SectionHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
    // Override highlighting and its revert are per entity.
    if (entityIds.size() == 1 && prefabOverrides(context, entityIds.front()).overridden("", "/local_transform")) {
        markPrefabOverride({});
        if (prefabOverrideMenu("##TransformOverride")) {
            context.invoke("prefab.revert_field", revertFieldProperties(entityIds.front(), "", "/local_transform"));
            return;
        }
    }
    if (!open || !matchesInspectorFilter("Transform", filter)) {
        return;
    }
    Scene& scene = context.app.scene;
    entt::registry& registry = scene.registry();
    // Every entity shown is edited (each has a transform, so Alt adds none):
    // the first one's values are shown, an axis whose values differ shows
    // mixedValueText, and an edit changes only that axis on every entity.
    std::vector<Entity> entities;
    std::vector<EditableTransform> transforms;
    size_t undecomposable = 0;
    for (const Entity entity : existingEntities(scene, entityIds)) {
        const auto* transform = registry.try_get<TransformComponent>(entity);
        EditableTransform editable;
        if (transform == nullptr) {
            continue;
        }
        if (!ComponentEditors::decomposeTransform(transform->local, editable)) {
            ++undecomposable;
            continue;
        }
        entities.push_back(entity);
        transforms.push_back(editable);
    }
    if (undecomposable > 0) {
        ImGui::TextColored(problemColor, entities.empty() ? "Transform cannot be decomposed."
                                                          : "Transforms that cannot be decomposed are not edited.");
    }
    if (entities.empty()) {
        return;
    }
    // Decomposing leaves float noise far below the shown precision, which
    // would mark equal values as mixed (a rotated entity's scale of 1 comes
    // back as 0.99999994). Within the tolerance a value is shown as the shown
    // entity's; only the axes an edit changes are written back, so the
    // others keep their exact values.
    const std::vector<EditableTransform> decomposed = transforms;
    for (EditableTransform& transform : std::span(transforms).subspan(1)) {
        alignTransformNoise(transforms.front(), transform);
    }
    const std::vector<EditableTransform> originals = transforms;
    std::vector<void*> objects;
    for (EditableTransform& transform : transforms) {
        objects.push_back(&transform);
    }
    if (!drawReflectedFields(objects, ComponentEditors::editableTransformFields())) {
        return;
    }
    // The scene is unedited so far, which is the snapshot undo needs.
    SceneEditing::EntitySnapshot before;
    if (!context.editing.captureEntities(entities, before)) {
        return;
    }
    for (size_t index = 0; index < entities.size(); ++index) {
        const EditableTransform edited = editedAxes(decomposed[index], originals[index], transforms[index]);
        // Untouched transforms keep their exact matrix rather than a
        // recomposed one.
        if (edited != decomposed[index]) {
            registry.get<TransformComponent>(entities[index]).local = ComponentEditors::composeTransform(edited);
        }
    }
    context.editing.commit("Edit Transform", entitiesCoalesceKey("transform:", scene, entities), std::move(before));
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
    if (!nameMatches && customEditor(context, type.key()) == nullptr &&
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
    // The shown (first) entity's values are the ones highlighted.
    const SceneEntityId shownId = scene.id(shown.front());
    std::optional<SceneEntityId> overriddenEntity;
    if (prefabOverrides(context, shownId).overridden(type.key(), "")) {
        markPrefabOverride({});
        overriddenEntity = shownId;
    }
    std::optional<HeaderAction> action;
    if (options.actions != HeaderActions::None) {
        action = drawComponentMenu(context, type, options, shownId, overriddenEntity);
    } else if (overriddenEntity && prefabOverrideMenu("##PrefabOverride")) {
        action = HeaderAction{.operatorId = "prefab.revert_field",
                              .properties = revertFieldProperties(*overriddenEntity, type.key(), "")};
    }
    if (open && !action) {
        action = drawComponentValues(context, type, shown, nameMatches ? std::string_view{} : filter);
    }
    if (action) {
        context.invoke(action->operatorId, action->properties);
    }
    ImGui::PopID();
}

std::optional<EntityPropertiesView::HeaderAction>
EntityPropertiesView::drawComponentMenu(EditorContext& context, const ComponentType& type,
                                        const ComponentDrawOptions& options, SceneEntityId shownEntity,
                                        std::optional<SceneEntityId> overriddenEntity) const {
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
        if (!context.editMode()) {
            // The whole selection shares a Selection component; a component
            // only the active entity has is applied for that entity alone.
            Json applyProperties{{"entity", ""}, {"component", type.key()}};
            if (!wholeSelection) {
                applyProperties["entity"] = std::to_string(shownEntity.value);
            }
            if (ImGui::MenuItem("Apply to Edit Scene", nullptr, false, context.canInvoke("play.apply_to_edit"))) {
                action = HeaderAction{.operatorId = "play.apply_to_edit", .properties = applyProperties.dump()};
            }
            ImGui::SetItemTooltip("Keep this component's running values after Stop, as one undoable edit");
            ImGui::TextDisabled("Other actions change the running scene only");
            ImGui::Separator();
        }
        if (overriddenEntity) {
            if (ImGui::MenuItem("Revert to Prefab")) {
                action = HeaderAction{.operatorId = "prefab.revert_field",
                                      .properties = revertFieldProperties(*overriddenEntity, type.key(), "")};
            }
            ImGui::Separator();
        }
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
    if (std::optional<HeaderAction> script = drawScriptControls(context, type)) {
        action = std::move(script);
    }
    return action;
}

std::optional<EntityPropertiesView::HeaderAction>
EntityPropertiesView::drawScriptControls(EditorContext& context, const ComponentType& type) const {
    if (context.scripts == nullptr) {
        return std::nullopt;
    }
    const std::optional<EditorSourceLocation> source = context.scripts->behaviourSource(type.key());
    if (!source) {
        return std::nullopt;
    }
    std::optional<HeaderAction> action;
    ImGui::SameLine();
    if (ImGui::SmallButton("Edit Script")) {
        action = HeaderAction{.operatorId = "module.edit_script", .properties = Json{{"component", type.key()}}.dump()};
    }
    ImGui::SetItemTooltip("%s:%u", utf8PathText(source->file).c_str(), source->line);
    drawScriptBadge(context.scripts->scriptStatus());
    return action;
}

std::optional<EntityPropertiesView::HeaderAction>
EntityPropertiesView::drawComponentValues(EditorContext& context, const ComponentType& type,
                                          std::span<const Entity> shown, std::string_view filter) const {
    Scene& scene = context.app.scene;
    std::optional<HeaderAction> revert;
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
    const CustomEditor* custom = customEditor(context, type.key());
    bool changed = false;
    if (custom != nullptr) {
        if (customValuesDiffer(context, type, shown)) {
            ImGui::TextDisabled("Values differ (%s); an edit applies only the changed setting.", mixedValueText);
        }
        changed = (*custom)(context, targets.front(), components.front());
        if (changed && targets.size() > 1) {
            copyCustomEdit(context, type, targets, originals.front());
        }
    } else {
        // The eyedropper fills the active entity's field; the multi-edit copy
        // below carries the change to the other targets.
        const SceneEntityId shownId = scene.id(targets.front());
        const EntityReferenceFieldContext entityReferences = entityReferenceFieldContext(context, shownId, type);
        // Copied: an edit below changes the revision the cache keys on.
        const PrefabEntityOverrides overrides = prefabOverrides(context, shownId);
        const std::optional<AssetReferenceFieldContext> assets = assetReferenceFieldContext(context);
        ReflectedFieldEditorOptions fieldOptions{
            .filter = filter,
            .assetLoaded =
                [&type, &scene, entity = targets.front()](std::string_view fieldKey) {
                    return type.assetLoaded(type.find(scene, entity), fieldKey);
                },
            .assets = assets ? &*assets : nullptr,
            .entityReferences = &entityReferences,
            .nameList =
                [&context](std::string_view listName) {
                    return context.projectNames != nullptr ? context.projectNames->list(listName) : nullptr;
                },
            .showMixedValues = targets.size() == shown.size()};
        if (overrides.linked && !overrides.added) {
            fieldOptions.overridden = [&overrides, &type](std::string_view pointer) {
                return overrides.overridden(type.key(), pointer);
            };
            fieldOptions.revertOverride = [&revert, &type, shownId](std::string_view pointer) {
                revert = HeaderAction{.operatorId = "prefab.revert_field",
                                      .properties = revertFieldProperties(shownId, type.key(), pointer)};
            };
        }
        if (context.watches != nullptr) {
            fieldOptions.watch = [&revert, &type, shownId](std::string_view pointer, std::string_view label) {
                revert = HeaderAction{.operatorId = "debug.watch_field",
                                      .properties = Json{{"entity", std::to_string(shownId.value)},
                                                         {"component", type.key()},
                                                         {"field", pointer},
                                                         {"label", type.displayName() + " " + std::string(label)}}
                                                        .dump()};
            };
        }
        changed = drawReflectedFields(components, type.fields(), fieldOptions);
    }
    if (changed) {
        // Copying a custom edit may have replaced the components, so look
        // each one up again. A changed asset path loads its asset first:
        // saving writes a loaded texture's own path, so a stale handle would
        // hide the edit from the undo step. In Play the edited value, handle
        // included, is what stays live.
        std::vector<std::any> edited;
        bool assetsLoaded = true;
        for (size_t index = 0; index < targets.size(); ++index) {
            void* component = type.find(scene, targets[index]);
            type.sanitize(component);
            if (const SceneDocumentResult reloaded =
                    type.reloadAssets(component, context.editing.assets(), context.editing.document().options());
                !reloaded && assetsLoaded) {
                context.diagnostics.reportError("Could not assign the asset: " + reloaded.error);
                assetsLoaded = false;
            }
            edited.push_back(type.copy(component));
            type.assign(component, originals[index]);
        }
        SceneEditing::EntitySnapshot before;
        if (assetsLoaded && context.editing.captureEntities(targets, before)) {
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
    return revert;
}

const EntityPropertiesView::CustomEditor* EntityPropertiesView::customEditor(const EditorContext& context,
                                                                             const std::string& componentKey) const {
    if (context.componentEditors != nullptr) {
        if (const CustomEditor* overriding = context.componentEditors->find(componentKey)) {
            return overriding;
        }
    }
    const auto builtIn = m_customEditors.find(componentKey);
    return builtIn == m_customEditors.end() ? nullptr : &builtIn->second;
}

void EntityPropertiesView::drawScriptBadge(ScriptStatus status) {
    const char* badge = scriptStatusBadge(status);
    if (badge == nullptr) {
        return;
    }
    ImGui::SameLine();
    const ThemeColor ink = status == ScriptStatus::Building ? palette::warning : palette::error;
    ImGui::TextColored(toImVec4(ink), "%s", badge);
    switch (status) {
    case ScriptStatus::Building:
        ImGui::SetItemTooltip("The game module is building; it reloads in Edit mode when the build succeeds.");
        break;
    case ScriptStatus::BuildFailed:
        ImGui::SetItemTooltip("The last build failed, so this behaviour runs its previous code. See the Build panel.");
        break;
    case ScriptStatus::ReloadFailed:
        ImGui::SetItemTooltip("The game module failed to load. Its data is kept until a reload succeeds; see "
                              "Diagnostics.");
        break;
    case ScriptStatus::Current:
        break;
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
        // A module that failed to load leaves its behaviours and components
        // here until a reload succeeds.
        if (context.scripts != nullptr && context.scripts->scriptStatus() == ScriptStatus::ReloadFailed) {
            drawScriptBadge(ScriptStatus::ReloadFailed);
        }
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

void EntityPropertiesView::drawModelField(EditorContext& context, SceneEntityId entityId) const {
    const Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(entityId);
    const std::optional<AssetReferenceFieldContext> assets = assetReferenceFieldContext(context);
    if (entity == entt::null || !assets || scene.registry().all_of<ModelNodeComponent>(entity)) {
        return;
    }
    std::string value;
    std::optional<bool> loaded;
    if (const auto* model = scene.registry().try_get<ModelComponent>(entity)) {
        loaded = context.app.assets.model(model->model) != nullptr;
        if (const std::optional<ModelAssetSource> source = context.app.assets.modelSource(model->model)) {
            value = projectAssetValue(assets->assetRoot, source->path).value_or(utf8PathText(source->path));
        }
    }
    const std::string shown = value;
    ImGui::PushID("##ModelField");
    ImGui::BeginDisabled(!context.canInvoke("object.model_replace"));
    if (drawAssetReferenceField("Model", value, "model", *assets, {.loaded = loaded, .clearable = false}) &&
        !value.empty() && value != shown) {
        context.invoke("object.model_replace", Json{{"entity", entityId.value}, {"asset", value}}.dump());
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}
