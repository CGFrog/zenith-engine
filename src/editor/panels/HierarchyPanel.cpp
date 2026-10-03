#include "HierarchyPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/operators/OutlinerOperators.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/outliner/OutlinerRowWidgets.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/EntityIcon.hpp"
#include "editor/theme/PixelPainter.hpp"
#include "engine/scene/Prefab.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace {
constexpr const char* entityPayload = sceneEntityDragPayload;

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

std::string collectionProperties(SceneCollectionId collection) {
    return nlohmann::json{{"collection", collection.value}}.dump();
}

EditorIcon assetIcon(EditorAssetKind kind) {
    switch (kind) {
    case EditorAssetKind::Texture:
        return EditorIcon::Texture;
    case EditorAssetKind::Audio:
        return EditorIcon::AudioClip;
    case EditorAssetKind::Scene:
        return EditorIcon::Scene;
    case EditorAssetKind::Model:
        break;
    }
    return EditorIcon::Mesh;
}

// A prefab path under prefabs/ named after the entity that no file uses yet.
std::string newPrefabPath(const Scene& scene, SceneEntityId entity, const std::filesystem::path& assetRoot) {
    const Entity handle = scene.findEntity(entity);
    std::string name = handle == entt::null ? std::string() : scene.registry().get<NameComponent>(handle).name;
    std::ranges::replace_if(
        name, [](char character) { return std::string_view(R"(<>:"/\|?*)").find(character) != std::string_view::npos; },
        '_');
    if (name.empty()) {
        name = "Prefab";
    }
    std::string path = "prefabs/" + name + ".prefab";
    for (int suffix = 2; !assetRoot.empty() && std::filesystem::exists(assetRoot / path); ++suffix) {
        path = "prefabs/" + name + " " + std::to_string(suffix) + ".prefab";
    }
    return path;
}

const SceneCollection* findCollection(const Scene& scene, SceneCollectionId id) {
    return scene.collections().find(id);
}
} // namespace

HierarchyPanel::HierarchyPanel(const ProjectAssets* projectAssets)
    : m_projectAssets(projectAssets) {}

const char* HierarchyPanel::title() const {
    return "Hierarchy";
}

bool HierarchyPanel::editModeOnly() const {
    return true;
}

std::string HierarchyPanel::areaState() const {
    return std::string(outlinerDisplayModeKey(m_mode));
}

void HierarchyPanel::restoreAreaState(std::string_view state) {
    if (const std::optional<OutlinerDisplayMode> mode = parseOutlinerDisplayMode(state)) {
        m_mode = *mode;
    }
}

OutlinerDisplayMode HierarchyPanel::displayMode() const {
    return m_mode;
}

void HierarchyPanel::setDisplayMode(OutlinerDisplayMode mode) {
    m_mode = mode;
}

const std::vector<SceneEntityId>& HierarchyPanel::displayedEntities() const {
    return m_lastDisplayed;
}

ImGuiID HierarchyPanel::entityRowId(ImGuiID windowId, SceneEntityId entity, uint64_t scope) {
    // Scope zero hashes exactly as ImGui::TreeNodeEx(pointer id) in the
    // window does, the id these rows have always had.
    const ImGuiID seed = scope == 0 ? windowId : ImHashData(&scope, sizeof(scope), windowId);
    const void* pointer = reinterpret_cast<const void*>(static_cast<uintptr_t>(entity.value));
    return ImHashData(&pointer, sizeof(pointer), seed);
}

ImGuiID HierarchyPanel::collectionRowId(ImGuiID windowId, SceneCollectionId collection) {
    return ImHashData(&collection.value, sizeof(collection.value), ImHashStr("collection", 0, windowId));
}

ImGuiID HierarchyPanel::restrictionToggleId(ImGuiID rowId, const char* column) {
    return ImHashStr(column, 0, rowId);
}

ImGuiID HierarchyPanel::uniqueRowId(ImGuiID id) {
    for (uint32_t repeat = 1; !m_drawnRows.insert(id).second; ++repeat) {
        id = ImHashData(&repeat, sizeof(repeat), id);
    }
    return id;
}

void HierarchyPanel::draw(EditorContext& context) {
    Scene& scene = context.app.scene;
    context.selection.removeMissing(scene);
    m_setAllOpen = std::exchange(m_requestedAllOpen, std::nullopt);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        context.actions.handleObjectKeys(context.app, KeymapRegion::Outliner);
    }
    drawHeader(context);
    ImGui::Separator();

    m_seed = ImGui::GetCurrentWindow()->IDStack.back();
    m_drawnRows.clear();
    m_displayed.clear();
    std::vector<OrphanAsset> orphans;
    if (m_mode == OutlinerDisplayMode::OrphanData) {
        orphans = orphanAssets(context);
    }
    const std::vector<OutlinerNode> rows =
        buildOutlinerTree(scene, {.mode = m_mode, .filter = m_filter, .orphanAssets = orphans});

    if (m_mode == OutlinerDisplayMode::ViewLayer || m_mode == OutlinerDisplayMode::Collections) {
        if (ImGui::Selectable("Scene Root", false, ImGuiSelectableFlags_AllowDoubleClick)) {
            context.selection.clear();
        }
        acceptEntityDrop(context, {});
    }
    drawRows(context, rows);
    if (rows.empty() && m_mode == OutlinerDisplayMode::Prefabs) {
        ImGui::TextDisabled("No prefab instances");
    } else if (rows.empty() && m_mode == OutlinerDisplayMode::OrphanData) {
        ImGui::TextDisabled("No orphan data");
    }
    if (m_mode == OutlinerDisplayMode::OrphanData && !m_orphans.error.empty()) {
        ImGui::TextDisabled("Unused assets unavailable: %s", m_orphans.error.c_str());
    }
    m_lastDisplayed = std::move(m_displayed);
    m_displayed.clear();
    m_setAllOpen.reset();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // The pressed row was not drawn when the button came up.
        m_pendingPlainClick.reset();
    }
}

void HierarchyPanel::drawHeader(EditorContext& context) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    const std::string current(outlinerDisplayModeLabel(m_mode));
    if (ImGui::BeginCombo("##DisplayMode", current.c_str())) {
        for (const OutlinerDisplayMode mode : outlinerDisplayModes) {
            const std::string label(outlinerDisplayModeLabel(mode));
            if (ImGui::Selectable(label.c_str(), mode == m_mode)) {
                m_mode = mode;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##HierarchyFilter", "Search...", &m_filter);

    // Create parents the new entity to the active one when exactly one is selected.
    operatorButton(context, "Create", "object.create_entity", R"({"placement":"child"})");
    ImGui::SameLine();
    operatorButton(context, "Duplicate", "object.duplicate");
    ImGui::SameLine();
    operatorButton(context, "Delete", "object.delete");
    ImGui::SameLine();
    operatorButton(context, "New Collection", "collection.new", R"({"link_selection":false})");
    ImGui::SameLine();
    ImGui::Checkbox("Preserve world", &m_preserveWorldOnReparent);
    if (m_mode == OutlinerDisplayMode::OrphanData && m_projectAssets != nullptr) {
        ImGui::SameLine();
        if (widgets::Button("Refresh")) {
            m_orphans.valid = false;
        }
    }
}

void HierarchyPanel::drawRows(EditorContext& context, const std::vector<OutlinerNode>& rows) {
    for (const OutlinerNode& row : rows) {
        drawNode(context, row);
    }
}

void HierarchyPanel::drawNode(EditorContext& context, const OutlinerNode& node) {
    switch (node.kind) {
    case OutlinerNodeKind::Entity:
        drawEntity(context, node);
        break;
    case OutlinerNodeKind::Collection:
        drawCollection(context, node);
        break;
    case OutlinerNodeKind::Prefab:
    case OutlinerNodeKind::Group:
        drawGroup(context, node);
        break;
    case OutlinerNodeKind::Asset:
        drawAsset(node);
        break;
    }
}

bool HierarchyPanel::treeRow(ImGuiID id, const OutlinerNode& node, ImGuiTreeNodeFlags flags) {
    flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    } else if (!m_filter.empty()) {
        // Searching opens every row leading to a match.
        ImGui::SetNextItemOpen(true);
    } else if (m_setAllOpen) {
        ImGui::SetNextItemOpen(*m_setAllOpen);
    }
    if (node.filterContext) {
        ImGui::PushStyleColor(ImGuiCol_Text, widgets::color(palette::inkMuted));
    }
    // The label starts with room for the row's icon.
    const std::string label = std::string(widgets::IconLabelPadding()) + node.label;
    const bool open = ImGui::TreeNodeBehavior(id, flags, label.c_str(), label.c_str() + label.size());
    if (node.filterContext) {
        ImGui::PopStyleColor();
    }
    return open;
}

void HierarchyPanel::drawChildren(EditorContext& context, const OutlinerNode& node, float guideX, float guideTop) {
    drawRows(context, node.children);
    pixel::dottedVerticalLine(ImGui::GetWindowDrawList(), guideX, guideTop,
                              ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y,
                              widgets::color(palette::indentGuide), editorArtPixel());
    ImGui::TreePop();
}

void HierarchyPanel::drawEntity(EditorContext& context, const OutlinerNode& node) {
    const Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(node.entity);
    if (entity == entt::null) {
        return;
    }
    const ImGuiID rowId = uniqueRowId(entityRowId(m_seed, node.entity, node.scope));
    const bool selected = context.selection.contains(node.entity);
    const bool open =
        treeRow(rowId, node, ImGuiTreeNodeFlags_AllowOverlap | (selected ? ImGuiTreeNodeFlags_Selected : 0));
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    decorateOutlinerRow(entityIcon(scene.registry(), entity), selected, node.children.empty());
    m_displayed.push_back(node.entity);
    // Where a dotted guide joins this row's children to it.
    const float guideX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() / 2.0f;
    const float guideTop = rowMax.y;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        // A plain press on a selected row may start dragging the selection,
        // so it selects only that row when released without a drag.
        if (selected && !io.KeyCtrl && !io.KeyShift && !context.eyedropperArmed()) {
            m_pendingPlainClick = rowId;
        } else {
            clickEntity(context, node.entity);
        }
    }
    if (m_pendingPlainClick == rowId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            clickEntity(context, node.entity);
        }
        m_pendingPlainClick.reset();
    }
    // The payload is the dragged row's id alone, which entity reference
    // fields accept too; outliner drop targets move the whole selection
    // when the dragged row is part of it.
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(entityPayload, &node.entity.value, sizeof(node.entity.value));
        const size_t count = selected ? context.selection.size() : 1;
        if (count > 1) {
            ImGui::Text("%s and %zu more", node.label.c_str(), count - 1);
        } else {
            ImGui::TextUnformatted(node.label.c_str());
        }
        ImGui::EndDragDropSource();
    }
    acceptEntityDrop(context, node.entity);

    ImGui::PushOverrideID(rowId);
    drawEntityContextMenu(context, node.entity);
    const auto* visibility = scene.registry().try_get<VisibilityComponent>(entity);
    const VisibilityComponent flags = visibility != nullptr ? *visibility : VisibilityComponent{};
    const std::array<RestrictionToggle, 3> toggles{{
        {"visible", EditorIcon::Viewport, !flags.hidden, "Show in viewports"},
        {"selectable", EditorIcon::ToolTweak, flags.selectable, "Selectable in viewports"},
        {"renderable", EditorIcon::Camera, flags.renderable, "Rendered (in the game too)"},
    }};
    const int toggled = drawRestrictionToggles(rowMin, rowMax, toggles);
    ImGui::PopID();
    if (toggled >= 0) {
        const RestrictionToggle& toggle = toggles[static_cast<size_t>(toggled)];
        context.invoke(
            "object.set_visibility",
            nlohmann::json{{"entity", node.entity.value}, {"flag", toggle.id}, {"value", !toggle.enabled}}.dump());
    }
    if (open) {
        drawChildren(context, node, guideX, guideTop);
    }
}

void HierarchyPanel::drawCollection(EditorContext& context, const OutlinerNode& node) {
    const SceneCollection* collection = findCollection(context.app.scene, node.collection);
    if (collection == nullptr) {
        return;
    }
    const ImGuiID rowId = uniqueRowId(collectionRowId(m_seed, node.collection));
    const bool open = treeRow(rowId, node, ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_DefaultOpen);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    decorateOutlinerRow(EditorIcon::Collection, false, node.children.empty());
    const float guideX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() / 2.0f;
    const float guideTop = rowMax.y;
    acceptEntityDrop(context, {}, node.collection);

    ImGui::PushOverrideID(rowId);
    drawCollectionContextMenu(context, node.collection);
    const std::array<RestrictionToggle, 3> toggles{{
        {"visible", EditorIcon::Viewport, collection->visible, "Show the collection in viewports"},
        {"selectable", EditorIcon::ToolTweak, collection->selectable, "Members are selectable in viewports"},
        {"renderable", EditorIcon::Camera, collection->renderable, "Members are rendered (in the game too)"},
    }};
    const int toggled = drawRestrictionToggles(rowMin, rowMax, toggles);
    ImGui::PopID();
    if (toggled >= 0) {
        const RestrictionToggle& toggle = toggles[static_cast<size_t>(toggled)];
        context.invoke(
            "collection.set_flag",
            nlohmann::json{{"collection", node.collection.value}, {"flag", toggle.id}, {"value", !toggle.enabled}}
                .dump());
    }
    if (open) {
        drawChildren(context, node, guideX, guideTop);
    }
}

void HierarchyPanel::drawGroup(EditorContext& context, const OutlinerNode& node) {
    const ImGuiID kind = ImHashStr(node.kind == OutlinerNodeKind::Prefab ? "prefab" : "group", 0, m_seed);
    const ImGuiID rowId = uniqueRowId(ImHashStr(node.label.c_str(), 0, kind));
    const bool open = treeRow(rowId, node, ImGuiTreeNodeFlags_DefaultOpen);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    decorateOutlinerRow(node.kind == OutlinerNodeKind::Prefab ? EditorIcon::Prefab : EditorIcon::Folder, false,
                        node.children.empty());
    if (open) {
        drawChildren(context, node, rowMin.x + ImGui::GetTreeNodeToLabelSpacing() / 2.0f, rowMax.y);
    }
}

void HierarchyPanel::drawAsset(const OutlinerNode& node) {
    const ImGuiID rowId = uniqueRowId(ImHashStr(node.path.c_str(), 0, ImHashStr("asset", 0, m_seed)));
    if (treeRow(rowId, node, ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        decorateOutlinerRow(assetIcon(node.assetKind), false, true);
    }
}

void HierarchyPanel::clickEntity(EditorContext& context, SceneEntityId entity) {
    // An armed eyedropper takes the click; the selection stays as it is.
    if (context.eyedropperArmed()) {
        context.pickEyedropperEntity(entity);
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const SelectionClickMode mode = io.KeyCtrl ? SelectionClickMode::Extend : SelectionClickMode::Replace;
    if (io.KeyShift) {
        const SceneEntityId anchor = m_rangeAnchor.value_or(context.selection.active().value_or(entity));
        context.selection.clickRange(m_lastDisplayed, anchor, entity, mode);
        return;
    }
    context.selection.click(entity, mode);
    m_rangeAnchor = entity;
}

void HierarchyPanel::acceptEntityDrop(EditorContext& context, SceneEntityId parent, SceneCollectionId collection) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(entityPayload);
        payload != nullptr && payload->DataSize == static_cast<int>(sizeof(uint64_t))) {
        uint64_t value = 0;
        std::memcpy(&value, payload->Data, sizeof(value));
        const SceneEntityId dragged{value};
        // Dragging a row outside the selection moves that row alone.
        if (!context.selection.contains(dragged)) {
            context.selection.selectOnly(dragged);
        }
        if (collection) {
            context.invoke("collection.link", collectionProperties(collection));
        } else {
            nlohmann::json properties{{"keep_transform", m_preserveWorldOnReparent}};
            properties["parent"] = parent ? nlohmann::json(parent.value) : nlohmann::json(nullptr);
            context.invoke("outliner.parent_drop", properties.dump());
        }
    }
    // A prefab dragged from the Asset Browser is instantiated under the row.
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(prefabAssetPayload)) {
        context.placePrefab(static_cast<const char*>(payload->Data), parent);
    }
    ImGui::EndDragDropTarget();
}

void HierarchyPanel::drawExpandItems() {
    if (ImGui::MenuItem("Expand All")) {
        m_requestedAllOpen = true;
    }
    if (ImGui::MenuItem("Collapse All")) {
        m_requestedAllOpen = false;
    }
}

void HierarchyPanel::drawEntityContextMenu(EditorContext& context, SceneEntityId entity) {
    if (context.eyedropperBlocksContextMenus() || !ImGui::BeginPopupContextItem("##EntityContext")) {
        return;
    }
    // As in Blender's outliner, the menu acts on the clicked entity: it keeps
    // a selection the entity is part of and otherwise selects only it.
    if (ImGui::IsWindowAppearing() && !context.selection.contains(entity)) {
        context.selection.selectOnly(entity);
    }
    const Scene& scene = context.app.scene;
    operatorMenuItem(context, "Create Child", "object.create_entity", R"({"placement":"child"})");
    operatorMenuItem(context, "Create Sibling", "object.create_entity", R"({"placement":"sibling"})");
    ImGui::Separator();
    operatorMenuItem(context, "Duplicate", "object.duplicate");
    operatorMenuItem(context, "Delete", "object.delete");
    ImGui::Separator();
    operatorMenuItem(context, "Rename", "object.rename_popup");
    ImGui::Separator();
    operatorMenuItem(context, "New Collection from Selection", "collection.new", R"({"link_selection":true})");
    const std::span<const SceneCollection> collections = scene.collections().all();
    if (ImGui::BeginMenu("Link to Collection", !collections.empty())) {
        for (const SceneCollection& collection : collections) {
            operatorMenuItem(context, collection.name.c_str(), "collection.link", collectionProperties(collection.id));
        }
        ImGui::EndMenu();
    }
    // Collections any selected entity is in.
    std::vector<SceneCollectionId> linked;
    for (const SceneEntityId id : context.selection.ids()) {
        const Entity handle = scene.findEntity(id);
        const auto* membership =
            handle == entt::null ? nullptr : scene.registry().try_get<CollectionMembershipComponent>(handle);
        if (membership == nullptr) {
            continue;
        }
        for (const SceneCollectionId collection : membership->collections) {
            if (std::ranges::find(linked, collection) == linked.end()) {
                linked.push_back(collection);
            }
        }
    }
    std::ranges::sort(linked);
    if (ImGui::BeginMenu("Unlink from Collection", !linked.empty())) {
        for (const SceneCollectionId id : linked) {
            if (const SceneCollection* collection = findCollection(scene, id)) {
                operatorMenuItem(context, collection->name.c_str(), "collection.unlink", collectionProperties(id));
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const nlohmann::json prefabPath{
        {"path", newPrefabPath(scene, entity, context.editing.document().options().assetRoot)}};
    operatorMenuItem(context, "Create Prefab", "prefab.create_from_selection", prefabPath.dump());
    operatorMenuItem(context, "Apply to Prefab", "prefab.apply");
    operatorMenuItem(context, "Revert to Prefab", "prefab.revert");
    operatorMenuItem(context, "Unpack Prefab", "prefab.unpack");
    ImGui::Separator();
    drawExpandItems();
    ImGui::EndPopup();
}

void HierarchyPanel::drawCollectionContextMenu(EditorContext& context, SceneCollectionId collection) {
    if (context.eyedropperBlocksContextMenus() || !ImGui::BeginPopupContextItem("##CollectionContext")) {
        return;
    }
    const std::string properties = collectionProperties(collection);
    operatorMenuItem(context, "New Collection", "collection.new",
                     nlohmann::json{{"parent", collection.value}, {"link_selection", false}}.dump());
    ImGui::Separator();
    operatorMenuItem(context, "Link Selection", "collection.link", properties);
    operatorMenuItem(context, "Unlink Selection", "collection.unlink", properties);
    ImGui::Separator();
    operatorMenuItem(context, "Instance to Scene", "object.collection_instance_add", properties);
    ImGui::Separator();
    operatorMenuItem(context, "Delete Collection", "collection.delete", properties);
    ImGui::Separator();
    drawExpandItems();
    ImGui::EndPopup();
}

std::vector<OrphanAsset> HierarchyPanel::orphanAssets(EditorContext& context) {
    if (m_projectAssets == nullptr || !m_projectAssets->scanned()) {
        return {};
    }
    const std::vector<EditorAssetRecord>& assets = m_projectAssets->catalog().assets();
    const uint64_t revision = context.editing.historyRevision();
    const uint64_t moves = m_projectAssets->latestItemMoveSequence();
    if (m_orphans.valid && m_orphans.historyRevision == revision && m_orphans.assetCount == assets.size() &&
        m_orphans.moveSequence == moves) {
        return m_orphans.assets;
    }
    // A failure is cached too (and shown in the panel) so it is reported
    // once, until the document or the assets change or Refresh is pressed.
    m_orphans = {.valid = true, .historyRevision = revision, .assetCount = assets.size(), .moveSequence = moves};
    std::string document;
    if (!context.editing.capture(document)) {
        m_orphans.error = "the scene could not be serialized";
        return {};
    }
    SceneAssetUsage usage;
    if (std::string error; !collectSceneAssetUsage(document, usage, error)) {
        m_orphans.error = error;
        context.diagnostics.reportError("Orphan Data: " + error);
        return {};
    }
    m_orphans.assets = findOrphanAssets(assets, context.editing.document().options().assetRoot, usage);
    return m_orphans.assets;
}
