#include "HierarchyPanel.hpp"

#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/EntityIcon.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr const char* entityPayload = sceneEntityDragPayload;

// Pixel decoration of the tree row just drawn: the entity's icon in the
// label's reserved space, and for a selected row lit edges and, where no
// expand arrow is drawn, a cursor.
void decorateEntityRow(const entt::registry& registry, Entity entity, bool selected, bool leaf) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    const float labelOffset = ImGui::GetTreeNodeToLabelSpacing();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (selected) {
        drawList->AddRectFilled(min, {max.x, min.y + artPixel}, widgets::color(rowSelectedHighlight));
        drawList->AddRectFilled({min.x, max.y - artPixel}, max, widgets::color(rowSelectedShade));
        if (leaf) {
            const float cursor = artPixel * 6.0f;
            pixel::triangle(drawList, {min.x + (labelOffset - cursor) / 2.0f, (min.y + max.y - cursor) / 2.0f}, cursor,
                            ImGuiDir_Right, widgets::color(pixelGlow));
        }
    }
    widgets::ItemIcon(entityIcon(registry, entity), labelOffset);
}

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

void operatorButton(EditorContext& context, const char* label, std::string_view operatorId,
                    std::string_view properties = {}) {
    ImGui::BeginDisabled(!context.canInvoke(operatorId));
    if (widgets::Button(label)) {
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
    // A prefab dragged from the Asset Browser is instantiated under the row.
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(prefabAssetPayload)) {
        context.placePrefab(static_cast<const char*>(payload->Data), parent);
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
    // The label starts with room for the entity's icon.
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entityId.value)), flags, "%s%s",
                                        widgets::IconLabelPadding(), name.empty() ? "Unnamed Entity" : name.c_str());
    decorateEntityRow(registry, entity, (flags & ImGuiTreeNodeFlags_Selected) != 0,
                      (flags & ImGuiTreeNodeFlags_Leaf) != 0);
    // Where a dotted guide joins this row's children to it.
    const float guideX = ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing() / 2.0f;
    const float guideTop = ImGui::GetItemRectMax().y;
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        // An armed eyedropper takes the click; the selection stays as it is.
        if (context.eyedropperArmed()) {
            context.pickEyedropperEntity(entityId);
        } else {
            context.selection.click(entityId,
                                    ImGui::GetIO().KeyCtrl ? SelectionClickMode::Extend : SelectionClickMode::Replace);
        }
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
        pixel::dottedVerticalLine(ImGui::GetWindowDrawList(), guideX, guideTop,
                                  ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y,
                                  widgets::color(palette::indentGuide), editorArtPixel());
        ImGui::TreePop();
    }
}

void HierarchyPanel::drawContextMenu(EditorContext& context, Entity entity) {
    if (context.eyedropperBlocksContextMenus() || !ImGui::BeginPopupContextItem("HierarchyContextMenu")) {
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
