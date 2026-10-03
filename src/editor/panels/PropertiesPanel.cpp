#include "PropertiesPanel.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/RenderSettingsControls.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace {
// Area state of a Components tab: the prefix followed by the category label.
constexpr std::string_view componentsStatePrefix = "components:";

const ComponentTypeRegistry& componentTypes(const EditorContext& context) {
    return componentTypesFor(context.editing.document().options());
}

// The single selected entity, when exactly one exists.
std::optional<SceneEntityId> singleSelection(const EditorContext& context) {
    if (context.selection.size() != 1) {
        return std::nullopt;
    }
    const std::optional<SceneEntityId> active = context.selection.active();
    if (!active || context.app.scene.findEntity(*active) == entt::null) {
        return std::nullopt;
    }
    return active;
}

// The entity the World tab shows for Component: the one the renderer uses
// (the primary component with the lowest entity handle, see MeshPass), or,
// when none is primary, the lowest-handle one so it can be made primary again.
template <typename Component> std::optional<SceneEntityId> worldEntity(const Scene& scene) {
    const auto components = scene.registry().view<const Component>();
    Entity selected = entt::null;
    bool selectedPrimary = false;
    for (const Entity entity : components) {
        const bool primary = components.template get<const Component>(entity).primary;
        const bool lower = selected == entt::null || entt::to_integral(entity) < entt::to_integral(selected);
        if ((primary && !selectedPrimary) || (primary == selectedPrimary && lower)) {
            selected = entity;
            selectedPrimary = primary;
        }
    }
    if (selected == entt::null) {
        return std::nullopt;
    }
    return scene.id(selected);
}

// Every irradiance volume, ordered by stable id so the list does not reorder
// after undo recreates entities.
std::vector<SceneEntityId> irradianceVolumes(const Scene& scene) {
    std::vector<SceneEntityId> volumes;
    for (const Entity entity : scene.registry().view<const IrradianceVolumeComponent>()) {
        volumes.push_back(scene.id(entity));
    }
    std::ranges::sort(volumes, {}, &SceneEntityId::value);
    return volumes;
}

bool isEntityTab(PropertiesTabKind kind) {
    return kind == PropertiesTabKind::Object || kind == PropertiesTabKind::Components;
}
} // namespace

std::string PropertiesTab::label() const {
    switch (kind) {
    case PropertiesTabKind::Object:
        return "Object";
    case PropertiesTabKind::Components:
        return category;
    case PropertiesTabKind::World:
        return "World";
    case PropertiesTabKind::Render:
        return "Render";
    case PropertiesTabKind::Scene:
        return "Scene";
    }
    return {};
}

PropertiesPanel::PropertiesPanel(RenderSettingsController& renderSettings)
    : m_renderSettings(renderSettings) {}

const char* PropertiesPanel::title() const {
    return "Properties";
}

std::vector<PropertiesTab> PropertiesPanel::tabs(const EditorContext& context) const {
    std::vector<PropertiesTab> result{{.kind = PropertiesTabKind::Object}};
    if (const std::optional<SceneEntityId> entity = singleSelection(context)) {
        for (std::string& category : entityComponentCategories(componentTypes(context), context.app.scene, *entity)) {
            result.push_back({.kind = PropertiesTabKind::Components, .category = std::move(category)});
        }
    }
    result.push_back({.kind = PropertiesTabKind::World});
    result.push_back({.kind = PropertiesTabKind::Render});
    result.push_back({.kind = PropertiesTabKind::Scene});
    return result;
}

PropertiesTab PropertiesPanel::activeTab(const EditorContext& context) const {
    const std::vector<PropertiesTab> available = tabs(context);
    if (std::ranges::find(available, m_selected) != available.end()) {
        return m_selected;
    }
    return {.kind = PropertiesTabKind::Object};
}

void PropertiesPanel::selectTab(PropertiesTab tab) {
    m_selected = std::move(tab);
}

std::string PropertiesPanel::areaState() const {
    switch (m_selected.kind) {
    case PropertiesTabKind::Object:
        return "object";
    case PropertiesTabKind::Components:
        return std::string(componentsStatePrefix) + m_selected.category;
    case PropertiesTabKind::World:
        return "world";
    case PropertiesTabKind::Render:
        return "render";
    case PropertiesTabKind::Scene:
        return "scene";
    }
    return {};
}

void PropertiesPanel::restoreAreaState(std::string_view state) {
    if (state.starts_with(componentsStatePrefix) && state.size() > componentsStatePrefix.size()) {
        selectTab({.kind = PropertiesTabKind::Components,
                   .category = std::string(state.substr(componentsStatePrefix.size()))});
        return;
    }
    constexpr std::array<std::pair<std::string_view, PropertiesTabKind>, 4> kinds{{
        {"object", PropertiesTabKind::Object},
        {"world", PropertiesTabKind::World},
        {"render", PropertiesTabKind::Render},
        {"scene", PropertiesTabKind::Scene},
    }};
    for (const auto& [name, kind] : kinds) {
        if (state == name) {
            selectTab({.kind = kind});
            return;
        }
    }
}

void PropertiesPanel::draw(EditorContext& context) {
    context.selection.removeMissing(context.app.scene);
    const std::vector<PropertiesTab> available = tabs(context);
    const PropertiesTab active = activeTab(context);
    drawTabStrip(available, active);
    ImGui::SameLine();
    if (ImGui::BeginChild("##PropertiesContent")) {
        // Play mode runs on a throwaway copy, so authoring tabs are read-only.
        const bool authoring = isEntityTab(active.kind) || active.kind == PropertiesTabKind::World;
        ImGui::BeginDisabled(authoring && !context.editMode());
        switch (active.kind) {
        case PropertiesTabKind::Object:
            drawObjectTab(context);
            break;
        case PropertiesTabKind::Components:
            drawComponentsTab(context, active.category);
            break;
        case PropertiesTabKind::World:
            drawWorldTab(context);
            break;
        case PropertiesTabKind::Render:
            drawRenderTab(context);
            break;
        case PropertiesTabKind::Scene:
            drawSceneTab(context);
            break;
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

void PropertiesPanel::drawTabStrip(const std::vector<PropertiesTab>& tabs, const PropertiesTab& active) {
    float labelWidth = 0.0f;
    for (const PropertiesTab& tab : tabs) {
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(tab.label().c_str()).x);
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float maximumWidth = ImGui::GetContentRegionAvail().x * 0.4f;
    const float width = std::min(labelWidth + (style.FramePadding.x + style.WindowPadding.x) * 2.0f, maximumWidth);
    if (ImGui::BeginChild("##PropertiesTabs", {width, 0.0f}, ImGuiChildFlags_Borders)) {
        for (size_t index = 0; index < tabs.size(); ++index) {
            const PropertiesTab& tab = tabs[index];
            // Entity tabs are grouped apart from the scene-wide ones.
            if (index > 0 && isEntityTab(tabs[index - 1].kind) && !isEntityTab(tab.kind)) {
                ImGui::Separator();
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(tab.label().c_str(), tab == active)) {
                selectTab(tab);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void PropertiesPanel::drawObjectTab(EditorContext& context) {
    if (context.selection.empty()) {
        ImGui::TextDisabled("Select an entity to edit its properties.");
        return;
    }
    const std::optional<SceneEntityId> entityId = singleSelection(context);
    if (!entityId) {
        m_view.drawMultipleSelection(context);
        return;
    }
    m_view.drawName(context, *entityId);
    ImGui::Separator();
    m_view.drawFilter("##PropertiesFilter", m_filter);
    m_view.drawTransform(context, *entityId, m_filter);
    m_view.drawPreservedComponents(context, *entityId);
    m_view.drawAssetWarnings(context, *entityId);
    m_view.drawModelDropTarget(context, *entityId);
    ImGui::Separator();
    m_view.drawAddComponentMenu(context, *entityId);
}

void PropertiesPanel::drawComponentsTab(EditorContext& context, const std::string& category) {
    const std::optional<SceneEntityId> entityId = singleSelection(context);
    if (!entityId) {
        return;
    }
    ImGui::TextUnformatted(
        context.app.scene.registry().get<NameComponent>(context.app.scene.findEntity(*entityId)).name.c_str());
    m_view.drawFilter("##PropertiesFilter", m_filter);
    m_view.drawComponents(context, *entityId, m_filter, category);
    // The bake button belongs beside the volume it bakes.
    if (const ComponentType* volume = componentTypes(context).find("irradiance_volume");
        volume != nullptr && componentCategoryLabel(*volume) == category) {
        m_view.drawIrradianceBakeButton(context, *entityId);
    }
    ImGui::Separator();
    m_view.drawAddComponentMenu(context, *entityId, category);
}

void PropertiesPanel::drawWorldTab(EditorContext& context) {
    Scene& scene = context.app.scene;
    const ComponentTypeRegistry& types = componentTypes(context);
    m_view.drawFilter("##PropertiesFilter", m_filter);

    // Draws one world component of another entity. Its header has no Remove,
    // since component removal acts on the active object.
    const auto drawWorldComponent = [&](std::string_view key, SceneEntityId entityId) {
        const ComponentType* type = types.find(key);
        const Entity entity = scene.findEntity(entityId);
        if (type == nullptr || entity == entt::null) {
            return;
        }
        // One entity may hold several world components.
        ImGui::PushID((std::to_string(entityId.value) + ':' + std::string(key)).c_str());
        ImGui::TextDisabled("%s", scene.registry().get<NameComponent>(entity).name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select")) {
            context.selection.selectOnly(entityId);
        }
        m_view.drawComponent(context, *type, entityId, {.filter = m_filter, .removable = false});
        ImGui::PopID();
    };

    ImGui::SeparatorText("Sun");
    if (const std::optional<SceneEntityId> sun = worldEntity<DirectionalLightComponent>(scene)) {
        drawWorldComponent("directional_light", *sun);
    } else if (ImGui::Button("Add Sun")) {
        context.invoke("object.add_sun");
    }

    ImGui::SeparatorText("Environment");
    if (const std::optional<SceneEntityId> environment = worldEntity<EnvironmentLightComponent>(scene)) {
        drawWorldComponent("environment_light", *environment);
    } else if (ImGui::Button("Add Environment")) {
        context.invoke("object.add_environment");
    }

    ImGui::SeparatorText("Fog");
    if (const std::optional<SceneEntityId> fog = worldEntity<VolumetricFogComponent>(scene)) {
        drawWorldComponent("volumetric_fog", *fog);
    } else if (ImGui::Button("Add Fog")) {
        context.invoke("object.add_fog");
    }

    ImGui::SeparatorText("Irradiance Volumes");
    const std::vector<SceneEntityId> volumes = irradianceVolumes(scene);
    if (volumes.empty()) {
        ImGui::TextDisabled("No irradiance volumes. Add one to an entity from its Add Component menu.");
    }
    for (const SceneEntityId volume : volumes) {
        drawWorldComponent("irradiance_volume", volume);
    }
}

void PropertiesPanel::drawRenderTab(EditorContext& context) {
    RenderSettingsControls::drawStorage(context, m_renderSettings);
    RenderSettingsControls::drawSettings(context, m_renderSettings);
}

void PropertiesPanel::drawSceneTab(EditorContext& context) const {
    const EditorDocument& document = context.editing.document();
    const Scene& scene = context.app.scene;
    ImGui::SeparatorText("Document");
    ImGui::Text("Name: %s", document.displayName().c_str());
    if (document.path()) {
        ImGui::TextWrapped("File: %s", document.path()->string().c_str());
    } else {
        ImGui::TextDisabled("Not saved to a file yet");
    }
    ImGui::Text("Unsaved changes: %s", document.dirty() ? "Yes" : "No");
    ImGui::Text("Entities: %zu", scene.registry().view<const SceneEntityIdComponent>().size());

    ImGui::SeparatorText("Project");
    ImGui::TextWrapped("Asset root: %s", document.options().assetRoot.string().c_str());
    const ComponentTypeRegistry& types = componentTypes(context);
    const auto visibleTypes =
        std::ranges::count_if(types.types(), [](const ComponentType& type) { return !type.internal(); });
    ImGui::Text("Component types: %td (%s)", visibleTypes,
                document.options().componentTypes ? "engine and game module" : "engine built-ins");
}
