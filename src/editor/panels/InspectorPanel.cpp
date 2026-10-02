#include "InspectorPanel.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/panels/EditorContext.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace {
constexpr ImVec4 problemColor{1.0f, 0.35f, 0.35f, 1.0f};

// Properties for object.component_add and object.component_remove.
std::string componentProperties(std::string_view key) {
    return nlohmann::json{{"component", key}}.dump();
}

template <typename Component> InspectorPanel::CustomEditor typedEditor(bool (*draw)(Component&)) {
    return [draw](EditorContext&, Entity, void* component) { return draw(*static_cast<Component*>(component)); };
}

const ComponentTypeRegistry& componentTypes(const EditorContext& context) {
    return componentTypesFor(context.editing.document().options());
}
} // namespace

InspectorPanel::InspectorPanel() {
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

const char* InspectorPanel::title() const {
    return "Inspector";
}

bool InspectorPanel::editModeOnly() const {
    return true;
}

void InspectorPanel::draw(EditorContext& context) {
    context.selection.removeMissing(context.app.scene);
    if (context.selection.empty()) {
        ImGui::TextDisabled("Select an entity to inspect it.");
        return;
    }
    if (context.selection.size() != 1) {
        drawMultipleSelection(context);
        return;
    }
    const SceneEntityId entityId = *context.selection.active();
    const Entity entity = context.app.scene.findEntity(entityId);
    if (entity == entt::null) {
        return;
    }
    drawName(context, entity, entityId);
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##InspectorFilter", "Filter properties...", &m_filter);
    drawTransform(context, entityId);
    drawComponents(context, entityId);
}

void InspectorPanel::drawMultipleSelection(EditorContext& context) {
    ImGui::Text("%zu entities selected", context.selection.size());
    if (ImGui::Button("Delete All")) {
        context.invoke("object.delete");
    }
}

void InspectorPanel::drawName(EditorContext& context, Entity entity, SceneEntityId entityId) {
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

void InspectorPanel::drawTransform(EditorContext& context, SceneEntityId entityId) {
    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) ||
        !matchesInspectorFilter("Transform", m_filter)) {
        return;
    }
    Scene& scene = context.app.scene;
    TransformComponent& transform = scene.registry().get<TransformComponent>(scene.findEntity(entityId));
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

void InspectorPanel::drawComponents(EditorContext& context, SceneEntityId entityId) {
    Scene& scene = context.app.scene;
    for (const ComponentType& type : componentTypes(context).types()) {
        if (!type.internal() && type.has(scene, scene.findEntity(entityId))) {
            drawComponent(context, type, entityId);
        }
    }
    drawPreservedComponents(context, entityId);
    if (scene.registry().all_of<IrradianceVolumeComponent>(scene.findEntity(entityId)) &&
        ImGui::Button("Bake Irradiance Volume", {-1.0f, 0.0f})) {
        context.invoke("object.irradiance_volume_bake");
    }

    const Entity entity = scene.findEntity(entityId);
    drawAssetWarnings(context, entity);
    ImGui::Button("Drop Model Asset to Add Child", {-1.0f, 0.0f});
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            context.placeModel(static_cast<const char*>(payload->Data), entity);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    drawAddComponentMenu(context, entityId);
}

void InspectorPanel::drawComponent(EditorContext& context, const ComponentType& type, SceneEntityId entityId) {
    Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(entityId);
    const bool nameMatches = matchesInspectorFilter(type.displayName(), m_filter);
    ReflectedFieldEditorOptions options{.filter = nameMatches ? std::string_view{} : std::string_view(m_filter),
                                        .assetLoaded = [&type, &scene, entity](std::string_view fieldKey) {
                                            return type.textureLoaded(type.find(scene, entity), fieldKey);
                                        }};
    const auto custom = m_customEditors.find(type.key());
    if (!nameMatches && custom == m_customEditors.end() &&
        std::ranges::none_of(type.fields(), [this](const FieldDescriptor& field) {
            return matchesInspectorFilter(field.presentation.label, m_filter);
        })) {
        return;
    }

    ImGui::PushID(type.key().c_str());
    const bool open = ImGui::CollapsingHeader(type.displayName().c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine();
    const bool remove = ImGui::SmallButton("Remove");
    void* component = type.find(scene, entity);
    if (open && !remove) {
        // Widgets edit in place; undo needs a snapshot of the unedited value.
        const std::any original = type.copy(component);
        const bool changed = custom != m_customEditors.end() ? custom->second(context, entity, component)
                                                             : drawReflectedFields(component, type.fields(), options);
        if (changed) {
            type.sanitize(component);
            const std::any edited = type.copy(component);
            type.assign(component, original);
            std::string before;
            if (context.editing.capture(before)) {
                type.assign(component, edited);
                context.editing.commit("Edit " + type.displayName(),
                                       "component:" + std::to_string(entityId.value) + ':' + type.key(),
                                       std::move(before));
            }
        }
        if (const std::string problem = type.validate(type.find(scene, scene.findEntity(entityId))); !problem.empty()) {
            ImGui::TextColored(problemColor, "%s", problem.c_str());
        }
    }
    if (remove) {
        context.invoke("object.component_remove", componentProperties(type.key()));
    }
    ImGui::PopID();
}

void InspectorPanel::drawPreservedComponents(EditorContext& context, SceneEntityId entityId) {
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
        const bool open = ImGui::CollapsingHeader(header.c_str());
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

void InspectorPanel::drawAssetWarnings(EditorContext& context, Entity entity) {
    const entt::registry& registry = context.app.scene.registry();
    if (const auto* model = registry.try_get<ModelComponent>(entity);
        model != nullptr && context.app.assets.model(model->model) == nullptr) {
        ImGui::TextColored(problemColor, "Model asset reference is invalid or missing.");
    }
    if (const auto* mesh = registry.try_get<MeshRendererComponent>(entity);
        mesh != nullptr && context.app.assets.mesh(mesh->mesh) == nullptr) {
        ImGui::TextColored(problemColor, "Mesh asset reference is invalid or missing.");
    }
}

void InspectorPanel::drawAddComponentMenu(EditorContext& context, SceneEntityId entityId) {
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    if (!ImGui::BeginPopup("AddComponentPopup")) {
        return;
    }
    Scene& scene = context.app.scene;
    const ComponentTypeRegistry& types = componentTypes(context);
    // Grouped by category in the order categories first appear.
    std::vector<std::string_view> categories;
    for (const ComponentType& type : types.types()) {
        if (!type.internal() && std::ranges::find(categories, type.category()) == categories.end()) {
            categories.push_back(type.category());
        }
    }
    for (std::string_view category : categories) {
        ImGui::SeparatorText(category.empty() ? "Other" : std::string(category).c_str());
        for (const ComponentType& type : types.types()) {
            if (type.internal() || type.category() != category || type.has(scene, scene.findEntity(entityId))) {
                continue;
            }
            if (ImGui::MenuItem(type.displayName().c_str())) {
                context.invoke("object.component_add", componentProperties(type.key()));
            }
        }
    }
    ImGui::EndPopup();
}
