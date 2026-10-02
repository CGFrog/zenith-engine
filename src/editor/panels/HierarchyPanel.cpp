#include "HierarchyPanel.hpp"

#include "editor/panels/EditorContext.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr const char* entityPayload = "ZENITH_SCENE_ENTITY";

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

void operatorButton(EditorContext& context, const char* label, std::string_view operatorId,
                    std::string_view properties = {}) {
    ImGui::BeginDisabled(!context.canInvoke(operatorId));
    if (ImGui::Button(label)) {
        context.invoke(operatorId, properties);
    }
    ImGui::EndDisabled();
}

void operatorMenuItem(EditorContext& context, const char* label, std::string_view operatorId,
                      std::string_view properties = {}) {
    const std::string shortcut = context.shortcut(operatorId, properties);
    if (ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, context.canInvoke(operatorId))) {
        context.invoke(operatorId, properties);
    }
}
} // namespace

const char* HierarchyPanel::title() const {
    return "Hierarchy";
}

bool HierarchyPanel::editModeOnly() const {
    return true;
}

bool HierarchyPanel::matchesFilter(const std::string& name) const {
    return m_filter.empty() || lowercase(name).find(lowercase(m_filter)) != std::string::npos;
}

void HierarchyPanel::draw(EditorContext& context) {
    Scene& scene = context.app.scene;
    context.selection.removeMissing(scene);
    m_setAllOpen = std::exchange(m_requestedAllOpen, std::nullopt);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        context.actions.handleObjectKeys(context.app, KeymapRegion::Outliner);
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##HierarchyFilter", "Search...", &m_filter);

    // Create parents the new entity to the active one when exactly one is selected.
    operatorButton(context, "Create", "object.create_entity", R"({"placement":"child"})");
    ImGui::SameLine();
    operatorButton(context, "Duplicate", "object.duplicate");
    ImGui::SameLine();
    operatorButton(context, "Delete", "object.delete");
    ImGui::SameLine();
    ImGui::Checkbox("Preserve world", &m_preserveWorldOnReparent);

    ImGui::Separator();

    if (ImGui::Selectable("Scene Root", false, ImGuiSelectableFlags_AllowDoubleClick)) {
        context.selection.clear();
    }
    acceptReparentDrop(context, entt::null);

    auto view = scene.registry().view<RelationshipComponent>();
    for (Entity entity : view) {
        if (view.get<RelationshipComponent>(entity).parent == entt::null &&
            matchesFilter(scene.registry().get<NameComponent>(entity).name)) {
            drawEntity(context, entity);
        }
    }
    m_setAllOpen.reset();
}

void HierarchyPanel::acceptReparentDrop(EditorContext& context, Entity parent) const {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(entityPayload)) {
        const SceneEntityId dragged{*static_cast<const uint64_t*>(payload->Data)};
        Scene& scene = context.app.scene;
        const Entity child = scene.findEntity(dragged);
        const ReparentMode mode =
            m_preserveWorldOnReparent ? ReparentMode::PreserveWorldTransform : ReparentMode::PreserveLocalTransform;
        context.editing.perform("Reparent Entity", {}, [&] { return scene.reparent(child, parent, mode); });
    }
    ImGui::EndDragDropTarget();
}

void HierarchyPanel::drawEntity(EditorContext& context, Entity entity) {
    entt::registry& registry = context.app.scene.registry();
    const SceneEntityId entityId = context.app.scene.id(entity);
    const auto& relationship = registry.get<RelationshipComponent>(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (relationship.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (context.selection.contains(entityId)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const std::string& name = registry.get<NameComponent>(entity).name;
    if (m_setAllOpen) {
        ImGui::SetNextItemOpen(*m_setAllOpen);
    }
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entityId.value)), flags, "%s",
                                        name.empty() ? "Unnamed Entity" : name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        context.selection.click(entityId,
                                ImGui::GetIO().KeyCtrl ? SelectionClickMode::Extend : SelectionClickMode::Replace);
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(entityPayload, &entityId.value, sizeof(entityId.value));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    acceptReparentDrop(context, entity);
    drawContextMenu(context, entity);
    if (open) {
        // Copied because drawing a child may edit the scene.
        const std::vector<Entity> children = relationship.children;
        for (Entity child : children) {
            if (registry.valid(child)) {
                const std::string& childName = registry.get<NameComponent>(child).name;
                if (m_filter.empty() || childName.find(m_filter) != std::string::npos) {
                    drawEntity(context, child);
                }
            }
        }
        ImGui::TreePop();
    }
}

void HierarchyPanel::drawContextMenu(EditorContext& context, Entity entity) {
    if (!ImGui::BeginPopupContextItem("HierarchyContextMenu")) {
        return;
    }
    // As in Blender's outliner, the menu acts on the clicked entity: it keeps
    // a selection the entity is part of and otherwise selects only it.
    const SceneEntityId entityId = context.app.scene.id(entity);
    if (ImGui::IsWindowAppearing() && !context.selection.contains(entityId)) {
        context.selection.selectOnly(entityId);
    }
    operatorMenuItem(context, "Create Child", "object.create_entity", R"({"placement":"child"})");
    operatorMenuItem(context, "Create Sibling", "object.create_entity", R"({"placement":"sibling"})");
    ImGui::Separator();
    operatorMenuItem(context, "Duplicate", "object.duplicate");
    operatorMenuItem(context, "Delete", "object.delete");
    ImGui::Separator();
    operatorMenuItem(context, "Rename", "object.rename_popup");
    if (ImGui::MenuItem("Expand All")) {
        m_requestedAllOpen = true;
    }
    if (ImGui::MenuItem("Collapse All")) {
        m_requestedAllOpen = false;
    }
    ImGui::EndPopup();
}
