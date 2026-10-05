#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/outliner/OutlinerTree.hpp"
#include "engine/scene/Scene.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ProjectAssets;

// The scene outliner: a filterable tree in one of four display modes (View
// Layer, Collections, Prefabs, Orphan Data; see OutlinerTree.hpp), kept per
// area. Rows select (Ctrl extends, Shift selects the displayed range from
// the last clicked row), feed an armed eyedropper, drag onto entities to
// reparent the dragged selection and onto collections to link it, and toggle
// visibility, selectability, and rendering in restriction columns. Every
// edit runs an operator. In Play and Simulate it lists the running scene:
// rows still select, operators that allow it change the running scene only,
// and an entity's menu offers Apply to Edit Scene (play.apply_to_edit).
class HierarchyPanel final : public EditorPanel {
  public:
    // projectAssets lists the project's assets for Orphan Data; without it
    // (tests) Orphan Data lists empty collections only.
    explicit HierarchyPanel(const ProjectAssets* projectAssets = nullptr);

    const char* title() const override;
    void draw(EditorContext& context) override;
    // The display mode's key (see outlinerDisplayModeKey).
    std::string areaState() const override;
    void restoreAreaState(std::string_view state) override;

    OutlinerDisplayMode displayMode() const;
    void setDisplayMode(OutlinerDisplayMode mode);
    // Entity rows in the order the last frame drew them, as Shift click
    // ranges over them.
    const std::vector<SceneEntityId>& displayedEntities() const;

    // ImGui ids of rows, so tools and tests can address them. windowId is
    // the id of the window the panel draws in; scope is OutlinerNode::scope.
    static ImGuiID entityRowId(ImGuiID windowId, SceneEntityId entity, uint64_t scope = 0);
    static ImGuiID collectionRowId(ImGuiID windowId, SceneCollectionId collection);
    // A row's restriction toggle: "visible", "selectable", or "renderable".
    static ImGuiID restrictionToggleId(ImGuiID rowId, const char* column);

  private:
    void drawHeader(EditorContext& context);
    void drawRows(EditorContext& context, const std::vector<OutlinerNode>& rows);
    void drawNode(EditorContext& context, const OutlinerNode& node);
    void drawEntity(EditorContext& context, const OutlinerNode& node);
    void drawCollection(EditorContext& context, const OutlinerNode& node);
    void drawGroup(EditorContext& context, const OutlinerNode& node);
    void drawAsset(const OutlinerNode& node);
    void drawChildren(EditorContext& context, const OutlinerNode& node, float guideX, float guideTop);
    // Opens a tree row with a precomputed id; returns whether it is open.
    bool treeRow(ImGuiID id, const OutlinerNode& node, ImGuiTreeNodeFlags flags);
    // An id for a row not drawn yet this frame, even when a tree lists the
    // same entity twice in one scope.
    ImGuiID uniqueRowId(ImGuiID id);

    void clickEntity(EditorContext& context, SceneEntityId entity);
    void drawEntityContextMenu(EditorContext& context, SceneEntityId entity);
    void drawCollectionContextMenu(EditorContext& context, SceneCollectionId collection);
    void drawExpandItems();
    // Accepts a dragged row: parents the dragged objects to parent (none for
    // the scene root), or links them to collection when it is set.
    void acceptEntityDrop(EditorContext& context, SceneEntityId parent, SceneCollectionId collection = {});
    // Orphan Data's unused assets, refreshed when the document or the
    // project's assets change.
    std::vector<OrphanAsset> orphanAssets(EditorContext& context);

    const ProjectAssets* m_projectAssets{nullptr};
    OutlinerDisplayMode m_mode{OutlinerDisplayMode::ViewLayer};
    std::string m_filter;
    bool m_preserveWorldOnReparent{true};
    // Expand All / Collapse All request opening or closing every tree node;
    // the request applies for the whole of the next frame.
    std::optional<bool> m_requestedAllOpen;
    std::optional<bool> m_setAllOpen;

    // Per frame: the id the panel's rows hash from, rows drawn so far, and
    // entity rows in draw order (moved to m_lastDisplayed at the end).
    ImGuiID m_seed{0};
    std::unordered_set<ImGuiID> m_drawnRows;
    std::vector<SceneEntityId> m_displayed;
    std::vector<SceneEntityId> m_lastDisplayed;
    // The row a Shift click ranges from: the last row clicked without Shift.
    std::optional<SceneEntityId> m_rangeAnchor;
    // The row (by id, since an entity may have several) of a selected entity
    // pressed without modifiers; it selects the entity alone on release
    // unless the press became a drag.
    std::optional<ImGuiID> m_pendingPlainClick;
    // The scene content generation the two above belong to.
    uint64_t m_sceneGeneration{0};

    struct OrphanCache {
        bool valid{false};
        uint64_t historyRevision{0};
        size_t assetCount{0};
        uint64_t moveSequence{0};
        std::vector<OrphanAsset> assets;
        // Why the assets could not be found; empty on success.
        std::string error;
    };

    OrphanCache m_orphans;
};
