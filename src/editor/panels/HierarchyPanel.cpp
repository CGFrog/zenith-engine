#include "HierarchyPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabEditDocument.hpp"
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
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace {
constexpr const char* entityPayload = sceneEntityDragPayload;

// While the running scene is shown the rows are rebuilt at least this often
// (in frames), since game code changes it without history.
constexpr uint64_t runtimeRefreshFrames = 30;

// A drop in this fraction of a row's height at its top or bottom edge lands
// between rows; the middle drops onto the row.
constexpr float reorderEdgeFraction = 0.25f;

constexpr const char* filterHelp = "Search names, or filter with:\n"
                                   "  c:light    has a component (key or name)\n"
                                   "  t:enemy    has a tag\n"
                                   "  p:door     is an instance of a prefab\n"
                                   "Terms combine (all must match); quote spaces: c:\"rigid body\"";

void operatorButton(EditorContext& context, const char* label, std::string_view operatorId,
                    std::string_view properties = {}) {
    ImGui::BeginDisabled(!context.canInvoke(operatorId));
    if (widgets::Button(label)) {
        context.invoke(operatorId, properties);
    }
    ImGui::EndDisabled();
}

void operatorMenuItem(EditorContext& context, const char* label, std::string_view operatorId,
                      std::string_view properties = {}, bool enabled = true) {
    const std::string shortcut = context.shortcut(operatorId, properties);
    if (ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false,
                        enabled && context.canInvoke(operatorId))) {
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
    case EditorAssetKind::Script:
        return EditorIcon::Script;
    case EditorAssetKind::Material:
        return EditorIcon::Environment;
    case EditorAssetKind::Data:
        return EditorIcon::Statistics;
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

bool hasChildren(const OutlinerRowEntry& entry) {
    return !entry.node->children.empty();
}
} // namespace

HierarchyPanel::HierarchyPanel(const ProjectAssets* projectAssets)
    : m_projectAssets(projectAssets) {}

const char* HierarchyPanel::title() const {
    return "Hierarchy";
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

const OutlinerRowCache::Stats& HierarchyPanel::rowCacheStats() const {
    return m_rows.stats();
}

std::optional<SceneCollectionId> HierarchyPanel::renamingCollection() const {
    return m_rename ? std::optional(m_rename->collection) : std::nullopt;
}

ImGuiID HierarchyPanel::entityRowId(ImGuiID windowId, SceneEntityId entity, uint64_t scope) {
    return outlinerEntityRowId(windowId, entity, scope);
}

ImGuiID HierarchyPanel::collectionRowId(ImGuiID windowId, SceneCollectionId collection) {
    return outlinerCollectionRowId(windowId, collection);
}

ImGuiID HierarchyPanel::restrictionToggleId(ImGuiID rowId, const char* column) {
    return ImHashStr(column, 0, rowId);
}

void HierarchyPanel::draw(EditorContext& context) {
    Scene& scene = context.app.scene;
    ++m_frames;
    context.selection.removeMissing(scene);
    if (scene.contentGeneration() != m_sceneGeneration) {
        // Another document (prefab mode, a scene opened): its ids name other
        // entities, so a range, a pending click, or a rename must not carry
        // over.
        m_sceneGeneration = scene.contentGeneration();
        m_rangeAnchor.reset();
        m_pendingPlainClick.reset();
        m_rename.reset();
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !m_rename) {
        // F2 over a collection renames it; over an object it is the keymap's.
        if (m_hoveredCollection && context.editMode() && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            beginCollectionRename(context, m_hoveredCollection->row, m_hoveredCollection->collection);
        } else {
            context.actions.handleObjectKeys(context.app, KeymapRegion::Outliner);
        }
    }
    m_hoveredCollection.reset();
    if (!context.editMode()) {
        widgets::CaptionText("Running scene: changes are lost on Stop", palette::warning);
    }
    drawHeader(context);
    ImGui::Separator();

    m_seed = ImGui::GetCurrentWindow()->IDStack.back();
    updateRows(context);

    if (m_mode == OutlinerDisplayMode::ViewLayer || m_mode == OutlinerDisplayMode::Collections) {
        if (ImGui::Selectable("Scene Root", false, ImGuiSelectableFlags_AllowDoubleClick)) {
            context.selection.clear();
        }
        acceptEntityDrop(context, {});
    }
    drawRows(context);
    const bool empty = m_rows.visible().empty();
    if (empty && m_mode == OutlinerDisplayMode::Prefabs) {
        ImGui::TextDisabled("No prefab instances");
    } else if (empty && m_mode == OutlinerDisplayMode::OrphanData) {
        ImGui::TextDisabled("No orphan data");
    }
    if (m_mode == OutlinerDisplayMode::OrphanData && !m_orphans.error.empty()) {
        ImGui::TextDisabled("Unused assets unavailable: %s", m_orphans.error.c_str());
    }
    m_knownActive = context.selection.active();
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
    const ImGuiStyle& style = ImGui::GetStyle();
    const float optionsWidth = ImGui::CalcTextSize("Filter").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(optionsWidth + style.ItemSpacing.x));
    ImGui::InputTextWithHint("##HierarchyFilter", "Search...", &m_filter);
    ImGui::SetItemTooltip("%s", filterHelp);
    ImGui::SameLine();
    drawFilterOptions(context);

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

void HierarchyPanel::drawFilterOptions(EditorContext& context) {
    if (widgets::Button("Filter")) {
        ImGui::OpenPopup("##FilterOptions");
    }
    ImGui::SetItemTooltip("Search syntax and Select All Matching");
    if (!ImGui::BeginPopup("##FilterOptions")) {
        return;
    }
    ImGui::TextUnformatted(filterHelp);
    ImGui::Separator();
    operatorMenuItem(context, "Select All Matching", "outliner.select_matching",
                     nlohmann::json{{"filter", m_filter}}.dump(), !m_filter.empty());
    ImGui::EndPopup();
}

void HierarchyPanel::updateRows(EditorContext& context) {
    const Scene& scene = context.app.scene;
    std::vector<OrphanAsset> orphans;
    if (m_mode == OutlinerDisplayMode::OrphanData) {
        orphans = orphanAssets(context);
    }
    const OutlinerRowSource source{
        .mode = m_mode,
        .filter = m_filter,
        .seed = m_seed,
        .historyRevision = context.editing.historyRevision(),
        .contentGeneration = scene.contentGeneration(),
        .entityCount = scene.registry().view<const SceneEntityIdComponent>().size(),
        .orphanGeneration = m_mode == OutlinerDisplayMode::OrphanData ? m_orphanGeneration : 0,
        .refreshTick = context.editMode() ? 0 : m_frames / runtimeRefreshFrames,
    };
    m_rows.update(source, scene.collections(), [&] {
        return buildOutlinerTree(scene, {.mode = m_mode,
                                         .filter = m_filter,
                                         .componentTypes = &componentTypesFor(context.editing.document().options()),
                                         .orphanAssets = orphans});
    });
    revealActive(context);
    if (m_rows.updateVisible(m_openRevision, [this](const OutlinerRowEntry& entry) { return isOpen(entry); })) {
        m_lastDisplayed.clear();
        for (const uint32_t index : m_rows.visible()) {
            const OutlinerNode& node = *m_rows.entries()[index].node;
            if (node.kind == OutlinerNodeKind::Entity) {
                m_lastDisplayed.push_back(node.entity);
            }
        }
    }
}

void HierarchyPanel::revealActive(const EditorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    if (active == m_knownActive) {
        return;
    }
    m_knownActive = active;
    const std::optional<uint32_t> found = active ? m_rows.findEntity(*active) : std::nullopt;
    if (!found) {
        return;
    }
    const std::vector<OutlinerRowEntry>& entries = m_rows.entries();
    for (uint32_t parent = entries[*found].parent; parent != OutlinerRowEntry::noParent;
         parent = entries[parent].parent) {
        if (!isOpen(entries[parent])) {
            m_openRows[entries[parent].id] = true;
            ++m_openRevision;
        }
    }
    m_revealRow = entries[*found].id;
}

bool HierarchyPanel::isOpen(const OutlinerRowEntry& entry) const {
    if (!m_filter.empty()) {
        // Searching opens every row leading to a match.
        return true;
    }
    if (const auto found = m_openRows.find(entry.id); found != m_openRows.end()) {
        return found->second;
    }
    return entry.node->kind != OutlinerNodeKind::Entity;
}

void HierarchyPanel::setAllOpen(bool open) {
    for (const OutlinerRowEntry& entry : m_rows.entries()) {
        if (hasChildren(entry)) {
            m_openRows[entry.id] = open;
        }
    }
    ++m_openRevision;
}

void HierarchyPanel::drawRows(EditorContext& context) {
    const std::vector<uint32_t>& visible = m_rows.visible();
    m_rowsOrigin = ImGui::GetCursorScreenPos();
    m_rowsLocalY = ImGui::GetCursorPosY();
    if (m_revealRow && m_rowHeight > 0.0f) {
        if (const std::optional<uint32_t> position = m_rows.visibleIndex(*m_revealRow)) {
            const float top = m_rowsLocalY + static_cast<float>(*position) * m_rowHeight;
            const float scroll = ImGui::GetScrollY();
            const float height = ImGui::GetWindowHeight();
            if (top < scroll || top + m_rowHeight > scroll + height) {
                ImGui::SetScrollY(top - (height - m_rowHeight) / 2.0f);
            }
        }
        m_revealRow.reset();
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    // Rows with something in progress stay drawn while scrolled out of view:
    // the pressed or dragged row, an open context menu, and a rename.
    const ImGuiContext& imgui = *ImGui::GetCurrentContext();
    std::array<std::optional<ImGuiID>, 5> kept{
        imgui.ActiveId != 0 ? std::optional(imgui.ActiveId) : std::nullopt,
        imgui.DragDropActive ? std::optional(imgui.DragDropPayload.SourceId) : std::nullopt, m_pendingPlainClick,
        m_contextRow, m_rename ? std::optional(m_rename->row) : std::nullopt};
    for (const std::optional<ImGuiID>& id : kept) {
        if (const std::optional<uint32_t> position = id ? m_rows.visibleIndex(*id) : std::nullopt) {
            clipper.IncludeItemByIndex(static_cast<int>(*position));
        }
    }
    m_contextRowThisFrame.reset();
    while (clipper.Step()) {
        for (int position = clipper.DisplayStart; position < clipper.DisplayEnd; ++position) {
            drawRow(context, visible[static_cast<size_t>(position)]);
        }
    }
    if (clipper.ItemsHeight > 0.0f) {
        m_rowHeight = clipper.ItemsHeight;
    }
    m_contextRow = m_contextRowThisFrame;
}

void HierarchyPanel::drawRow(EditorContext& context, uint32_t entryIndex) {
    const OutlinerRowEntry& entry = m_rows.entries()[entryIndex];
    const float indent = static_cast<float>(entry.depth) * ImGui::GetStyle().IndentSpacing;
    if (indent > 0.0f) {
        ImGui::Indent(indent);
    }
    switch (entry.node->kind) {
    case OutlinerNodeKind::Entity:
        drawEntity(context, entry);
        break;
    case OutlinerNodeKind::Collection:
        drawCollection(context, entry);
        break;
    case OutlinerNodeKind::Prefab:
    case OutlinerNodeKind::Group:
        drawGroup(entry);
        break;
    case OutlinerNodeKind::Asset:
        drawAsset(entry);
        break;
    }
    drawGuides(entry);
    if (indent > 0.0f) {
        ImGui::Unindent(indent);
    }
}

void HierarchyPanel::drawGuides(const OutlinerRowEntry& entry) const {
    if (entry.depth == 0) {
        return;
    }
    // Each ancestor's guide runs down through every row of its subtree; a
    // row draws its stretch (from the gap above it to its bottom) with dots
    // on a grid fixed to the first row, so stretches join seamlessly.
    const float artPixel = editorArtPixel();
    const float period = artPixel * 2.0f;
    const float gapTop = m_rowMin.y - ImGui::GetStyle().ItemSpacing.y;
    const float top = m_rowsOrigin.y + std::ceil((gapTop - m_rowsOrigin.y) / period) * period;
    const float indent = ImGui::GetStyle().IndentSpacing;
    const float offset = ImGui::GetTreeNodeToLabelSpacing() / 2.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (uint32_t level = 0; level < entry.depth; ++level) {
        const float x = m_rowsOrigin.x + static_cast<float>(level) * indent + offset;
        pixel::dottedVerticalLine(drawList, x, top, m_rowMax.y, widgets::color(palette::indentGuide), artPixel);
    }
}

bool HierarchyPanel::treeRow(const OutlinerRowEntry& entry, ImGuiTreeNodeFlags flags) {
    const OutlinerNode& node = *entry.node;
    flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    const bool parent = hasChildren(entry);
    if (parent) {
        ImGui::SetNextItemOpen(isOpen(entry));
    } else {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    // Context rows of a search and generated entities are dimmed.
    const bool muted = node.filterContext || node.transient;
    if (muted) {
        ImGui::PushStyleColor(ImGuiCol_Text, widgets::color(palette::inkMuted));
    }
    // The label starts with room for the row's icon.
    const std::string label = std::string(widgets::IconLabelPadding()) + node.label;
    const bool open = ImGui::TreeNodeBehavior(entry.id, flags, label.c_str(), label.c_str() + label.size());
    if (muted) {
        ImGui::PopStyleColor();
    }
    m_rowMin = ImGui::GetItemRectMin();
    m_rowMax = ImGui::GetItemRectMax();
    if (parent && ImGui::IsItemToggledOpen() && m_filter.empty()) {
        m_openRows[entry.id] = open;
        ++m_openRevision;
    }
    if (node.transient) {
        ImGui::SetItemTooltip("Generated, not saved: a preview a generator built, left out of saves, undo, "
                              "copies, and Play");
    }
    return open;
}

void HierarchyPanel::drawEntity(EditorContext& context, const OutlinerRowEntry& entry) {
    const OutlinerNode& node = *entry.node;
    const Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(node.entity);
    if (entity == entt::null) {
        // Deleted by an operator earlier this frame; the rows rebuild next
        // frame, and the clipper needs the row's height until then.
        treeRow(entry, ImGuiTreeNodeFlags_None);
        return;
    }
    const ImGuiID rowId = entry.id;
    const bool selected = context.selection.contains(node.entity);
    treeRow(entry, ImGuiTreeNodeFlags_AllowOverlap | (selected ? ImGuiTreeNodeFlags_Selected : 0));
    const ImVec2 rowMin = m_rowMin;
    const ImVec2 rowMax = m_rowMax;
    // In prefab mode the document's root is the prefab itself.
    const bool prefabRoot = context.prefabEdit != nullptr && context.prefabEdit->root == node.entity;
    decorateOutlinerRow(prefabRoot ? EditorIcon::Prefab : entityIcon(scene.registry(), entity), selected,
                        node.children.empty());
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
    // Between the children of an entity row, drops reorder.
    std::optional<ReorderTarget> reorder;
    if (entry.parent != OutlinerRowEntry::noParent) {
        if (const OutlinerNode& parent = *m_rows.entries()[entry.parent].node;
            parent.kind == OutlinerNodeKind::Entity) {
            reorder = ReorderTarget{.entity = node.entity, .parent = parent.entity};
        }
    }
    acceptEntityDrop(context, node.entity, {}, reorder);

    ImGui::PushOverrideID(rowId);
    drawEntityContextMenu(context, rowId, node.entity);
    const auto* visibility = scene.registry().try_get<VisibilityComponent>(entity);
    const VisibilityComponent flags = visibility != nullptr ? *visibility : VisibilityComponent{};
    const std::array<RestrictionToggle, 3> toggles{{
        {"visible", EditorIcon::Viewport, !flags.hidden, "Show in viewports"},
        {"selectable", EditorIcon::ToolTweak, flags.selectable, "Selectable in viewports"},
        {"renderable", EditorIcon::Camera, flags.renderable, "Rendered (in the game too)"},
    }};
    const int toggled = drawRestrictionToggles(rowMin, rowMax, toggles);
    ImGui::PopID();
    m_rowMin = rowMin;
    m_rowMax = rowMax;
    if (toggled >= 0) {
        const RestrictionToggle& toggle = toggles[static_cast<size_t>(toggled)];
        context.invoke(
            "object.set_visibility",
            nlohmann::json{{"entity", node.entity.value}, {"flag", toggle.id}, {"value", !toggle.enabled}}.dump());
    }
}

void HierarchyPanel::drawCollection(EditorContext& context, const OutlinerRowEntry& entry) {
    const OutlinerNode& node = *entry.node;
    const SceneCollection* collection = findCollection(context.app.scene, node.collection);
    if (collection == nullptr) {
        treeRow(entry, ImGuiTreeNodeFlags_None);
        return;
    }
    const ImGuiID rowId = entry.id;
    treeRow(entry, ImGuiTreeNodeFlags_AllowOverlap);
    const ImVec2 rowMin = m_rowMin;
    const ImVec2 rowMax = m_rowMax;
    decorateOutlinerRow(EditorIcon::Collection, false, node.children.empty());
    if (ImGui::IsItemHovered()) {
        m_hoveredCollection = HoveredCollection{.row = rowId, .collection = node.collection};
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.editMode()) {
            beginCollectionRename(context, rowId, node.collection);
        }
    }
    acceptEntityDrop(context, {}, node.collection);

    ImGui::PushOverrideID(rowId);
    drawCollectionContextMenu(context, rowId, node.collection);
    if (m_rename && m_rename->row == rowId) {
        drawCollectionRename(context, rowMin, rowMax);
    }
    const std::array<RestrictionToggle, 3> toggles{{
        {"visible", EditorIcon::Viewport, collection->visible, "Show the collection in viewports"},
        {"selectable", EditorIcon::ToolTweak, collection->selectable, "Members are selectable in viewports"},
        {"renderable", EditorIcon::Camera, collection->renderable, "Members are rendered (in the game too)"},
    }};
    const int toggled = drawRestrictionToggles(rowMin, rowMax, toggles);
    ImGui::PopID();
    m_rowMin = rowMin;
    m_rowMax = rowMax;
    if (toggled >= 0) {
        const RestrictionToggle& toggle = toggles[static_cast<size_t>(toggled)];
        context.invoke(
            "collection.set_flag",
            nlohmann::json{{"collection", node.collection.value}, {"flag", toggle.id}, {"value", !toggle.enabled}}
                .dump());
    }
}

void HierarchyPanel::beginCollectionRename(EditorContext& context, ImGuiID rowId, SceneCollectionId collection) {
    const SceneCollection* found = findCollection(context.app.scene, collection);
    if (found == nullptr || !context.canInvoke("collection.rename")) {
        return;
    }
    m_rename = CollectionRename{.row = rowId, .collection = collection, .name = found->name};
}

void HierarchyPanel::drawCollectionRename(EditorContext& context, ImVec2 rowMin, ImVec2 rowMax) {
    // A text field over the row's label; the row's cursor stays where it is.
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float labelX =
        rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(widgets::IconLabelPadding()).x;
    ImGui::SetCursorScreenPos({labelX, rowMin.y});
    ImGui::SetNextItemWidth(std::max(rowMax.x - labelX - restrictionColumnsWidth(3), ImGui::GetFontSize() * 4.0f));
    if (!m_rename->wasActive) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool entered = ImGui::InputText("##CollectionRename", &m_rename->name,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool active = ImGui::IsItemActive();
    const bool deactivated = ImGui::IsItemDeactivated();
    ImGui::SetCursorScreenPos(cursor);
    m_rename->wasActive = m_rename->wasActive || active;
    m_rename->idleFrames = active ? 0 : m_rename->idleFrames + 1;
    // Enter or clicking away commits, as Blender's rename; Escape cancels.
    if (entered || (deactivated && !ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        const CollectionRename rename = *std::exchange(m_rename, std::nullopt);
        context.invoke("collection.rename",
                       nlohmann::json{{"collection", rename.collection.value}, {"name", rename.name}}.dump());
    } else if (deactivated || (!m_rename->wasActive && m_rename->idleFrames > 2)) {
        m_rename.reset();
    }
}

void HierarchyPanel::drawGroup(const OutlinerRowEntry& entry) {
    treeRow(entry, ImGuiTreeNodeFlags_None);
    decorateOutlinerRow(entry.node->kind == OutlinerNodeKind::Prefab ? EditorIcon::Prefab : EditorIcon::Folder, false,
                        entry.node->children.empty());
}

void HierarchyPanel::drawAsset(const OutlinerRowEntry& entry) {
    treeRow(entry, ImGuiTreeNodeFlags_None);
    decorateOutlinerRow(assetIcon(entry.node->assetKind), false, true);
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

void HierarchyPanel::acceptEntityDrop(EditorContext& context, SceneEntityId parent, SceneCollectionId collection,
                                      const std::optional<ReorderTarget>& reorder) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    DropPlacement placement = DropPlacement::Onto;
    if (reorder) {
        const float fraction = (ImGui::GetMousePos().y - itemMin.y) / std::max(itemMax.y - itemMin.y, 1.0f);
        placement = fraction < reorderEdgeFraction          ? DropPlacement::Before
                    : fraction > 1.0f - reorderEdgeFraction ? DropPlacement::After
                                                            : DropPlacement::Onto;
    }
    const ImGuiDragDropFlags peek =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(entityPayload, peek);
        payload != nullptr && payload->DataSize == static_cast<int>(sizeof(uint64_t))) {
        // Between rows a line shows where the object lands; onto a row, its
        // outline.
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
        const float thickness = editorArtPixel();
        if (placement == DropPlacement::Onto) {
            drawList->AddRect(itemMin, itemMax, color, 0.0f, 0, thickness);
        } else {
            const float y = placement == DropPlacement::Before ? itemMin.y : itemMax.y;
            drawList->AddRectFilled({itemMin.x, y - thickness}, {itemMax.x, y + thickness}, color);
        }
        if (payload->IsDelivery()) {
            uint64_t value = 0;
            std::memcpy(&value, payload->Data, sizeof(value));
            const SceneEntityId dragged{value};
            // Dragging a row outside the selection moves that row alone.
            if (!context.selection.contains(dragged)) {
                context.selection.selectOnly(dragged);
            }
            if (placement != DropPlacement::Onto) {
                dropBetween(context, dragged, *reorder, placement);
            } else if (collection) {
                context.invoke("collection.link", collectionProperties(collection));
            } else {
                nlohmann::json properties{{"keep_transform", m_preserveWorldOnReparent}};
                properties["parent"] = parent ? nlohmann::json(parent.value) : nlohmann::json(nullptr);
                context.invoke("outliner.parent_drop", properties.dump());
            }
        }
    }
    // A prefab dragged from the Asset Browser is instantiated under the row.
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(prefabAssetPayload)) {
        const Entity parentEntity = parent ? context.app.scene.findEntity(parent) : Entity{entt::null};
        context.placePrefab(static_cast<const char*>(payload->Data), parentEntity);
    }
    ImGui::EndDragDropTarget();
}

void HierarchyPanel::dropBetween(EditorContext& context, SceneEntityId dragged, const ReorderTarget& reorder,
                                 DropPlacement placement) {
    if (dragged == reorder.entity) {
        return;
    }
    const Scene& scene = context.app.scene;
    const Entity parent = scene.findEntity(reorder.parent);
    const Entity target = scene.findEntity(reorder.entity);
    if (parent == entt::null || target == entt::null) {
        return;
    }
    // The position among the siblings once the dragged object has left them.
    const Entity moved = scene.findEntity(dragged);
    size_t index = 0;
    for (const Entity sibling : scene.registry().get<RelationshipComponent>(parent).children) {
        if (sibling == target) {
            break;
        }
        if (sibling != moved) {
            ++index;
        }
    }
    if (placement == DropPlacement::After) {
        ++index;
    }
    context.invoke("outliner.reorder", nlohmann::json{{"entity", dragged.value},
                                                      {"parent", reorder.parent.value},
                                                      {"index", index},
                                                      {"keep_transform", m_preserveWorldOnReparent}}
                                           .dump());
}

void HierarchyPanel::drawExpandItems() {
    if (ImGui::MenuItem("Expand All")) {
        setAllOpen(true);
    }
    if (ImGui::MenuItem("Collapse All")) {
        setAllOpen(false);
    }
}

void HierarchyPanel::drawEntityContextMenu(EditorContext& context, ImGuiID rowId, SceneEntityId entity) {
    if (context.eyedropperBlocksContextMenus() || !ImGui::BeginPopupContextItem("##EntityContext")) {
        return;
    }
    m_contextRowThisFrame = rowId;
    // As in Blender's outliner, the menu acts on the clicked entity: it keeps
    // a selection the entity is part of and otherwise selects only it.
    if (ImGui::IsWindowAppearing() && !context.selection.contains(entity)) {
        context.selection.selectOnly(entity);
    }
    const Scene& scene = context.app.scene;
    if (!context.editMode()) {
        // Everything else here changes the running scene only.
        operatorMenuItem(context, "Apply to Edit Scene", "play.apply_to_edit");
        ImGui::TextDisabled("Other changes here are lost on Stop");
        ImGui::Separator();
    }
    operatorMenuItem(context, "Create Child", "object.create_entity", R"({"placement":"child"})");
    operatorMenuItem(context, "Create Sibling", "object.create_entity", R"({"placement":"sibling"})");
    ImGui::Separator();
    operatorMenuItem(context, "Duplicate", "object.duplicate");
    operatorMenuItem(context, "Delete", "object.delete");
    ImGui::Separator();
    operatorMenuItem(context, "Rename", "object.rename_popup");
    if (!m_filter.empty()) {
        operatorMenuItem(context, "Select All Matching", "outliner.select_matching",
                         nlohmann::json{{"filter", m_filter}}.dump());
    }
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
    operatorMenuItem(context, "Edit Prefab", "prefab.edit");
    ImGui::Separator();
    drawExpandItems();
    ImGui::EndPopup();
}

void HierarchyPanel::drawCollectionContextMenu(EditorContext& context, ImGuiID rowId, SceneCollectionId collection) {
    if (context.eyedropperBlocksContextMenus() || !ImGui::BeginPopupContextItem("##CollectionContext")) {
        return;
    }
    m_contextRowThisFrame = rowId;
    const std::string properties = collectionProperties(collection);
    operatorMenuItem(context, "New Collection", "collection.new",
                     nlohmann::json{{"parent", collection.value}, {"link_selection", false}}.dump());
    if (ImGui::MenuItem("Rename", "F2", false, context.canInvoke("collection.rename"))) {
        beginCollectionRename(context, rowId, collection);
    }
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
    ++m_orphanGeneration;
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
