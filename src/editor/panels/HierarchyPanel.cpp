#include "HierarchyPanel.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <span>
#include <vector>

namespace {
constexpr const char* entityPayload = "ZENITH_SCENE_ENTITY";

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
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
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        context.actions.handleObjectKeys(context.app, KeymapRegion::Outliner);
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##HierarchyFilter", "Search...", &m_filter);

    if (ImGui::Button("Create")) {
        const Entity parent =
            context.selection.size() == 1 ? scene.findEntity(*context.selection.active()) : entt::null;
        if (const SceneEntityId created = EntityCommands::createEntity(context.editing, parent)) {
            context.selection.selectOnly(created);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && context.selection.size() == 1) {
        EntityCommands::duplicateSelection(context.editing, context.selection);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && !context.selection.empty()) {
        EntityCommands::deleteSelection(context.editing, context.selection);
    }
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
    Scene& scene = context.app.scene;
    if (ImGui::MenuItem("Create Child")) {
        if (const SceneEntityId created = EntityCommands::createEntity(context.editing, entity)) {
            context.selection.selectOnly(created);
        }
    }
    if (ImGui::MenuItem("Create Sibling")) {
        const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
        if (const SceneEntityId created = EntityCommands::createEntity(context.editing, parent)) {
            context.selection.selectOnly(created);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", objectCommandLabel(ObjectCommand::Duplicate), false, true)) {
        EntityCommands::duplicateSelection(context.editing, context.selection);
    }
    if (ImGui::MenuItem("Delete", objectCommandLabel(ObjectCommand::Delete), false, true)) {
        const SceneEntityId entityId = scene.id(entity);
        EntityCommands::deleteEntities(context.editing, std::span(&entityId, 1), "Delete Entity");
        context.selection.clear();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, true)) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (ImGui::MenuItem("Expand All")) {
    }
    if (ImGui::MenuItem("Collapse All")) {
    }
    ImGui::EndPopup();
}
