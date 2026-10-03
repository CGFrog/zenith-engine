#include "AddComponentMenu.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <string>

namespace {
std::string componentProperties(std::string_view key) {
    return nlohmann::json{{"component", key}}.dump();
}
} // namespace

void drawAddComponentButton(EditorContext& context, SceneEntityId entity, std::optional<std::string_view> category) {
    const std::optional<SceneEntityId> active = context.selection.active();
    const bool enabled = active && *active == entity && context.canInvoke("wm.call_menu");
    ImGui::BeginDisabled(!enabled);
    if (widgets::Button("Add Component")) {
        nlohmann::json properties{{"menu", addComponentMenuId}};
        if (category) {
            properties["category"] = *category;
        }
        context.invoke("wm.call_menu", properties.dump());
    }
    ImGui::EndDisabled();
}

void addComponentEntries(const OperatorContext& context, std::vector<MenuEntry>& entries) {
    const Scene& scene = context.editing.scene();
    const std::optional<SceneEntityId> active = context.selection.active();
    const Entity entity = active ? scene.findEntity(*active) : entt::null;
    if (entity == entt::null) {
        return;
    }
    for (const ComponentType& type : componentTypesFor(context.editing.document().options()).types()) {
        if (type.internal() || type.has(scene, entity)) {
            continue;
        }
        entries.push_back({.label = type.displayName(),
                           .operatorId = "object.component_add",
                           .properties = componentProperties(type.key()),
                           .submenu = {},
                           .icon = std::nullopt,
                           .category = {componentCategoryLabel(type)}});
    }
}

void addEmptyWithComponentEntries(const OperatorContext& context, std::vector<MenuEntry>& entries) {
    for (const ComponentType& type : componentTypesFor(context.editing.document().options()).types()) {
        if (type.internal()) {
            continue;
        }
        entries.push_back({.label = type.displayName(),
                           .operatorId = "object.add_empty_with",
                           .properties = componentProperties(type.key()),
                           .submenu = {},
                           .icon = std::nullopt,
                           .category = {"Component", componentCategoryLabel(type)}});
    }
}
