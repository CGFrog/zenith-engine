#pragma once

// The Hierarchy (outliner) panel: a filterable tree in View Layer, Collections, Prefabs, or Orphan Data mode.

#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/outliner/OutlinerRows.hpp"
#include "editor/panels/outliner/OutlinerTree.hpp"
#include "engine/scene/Scene.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ProjectAssets;

// The scene outliner: a filterable tree in one of four display modes (View
// Layer, Collections, Prefabs, Orphan Data; see OutlinerTree.hpp), kept per
// area. Rows select (Ctrl extends, Shift selects the displayed range from
// the last clicked row), feed an armed eyedropper, drag onto entities to
// reparent the dragged selection, between an entity's children to reorder
// the dragged one (outliner.reorder), and onto collections to link it, and
// toggle visibility, selectability, and rendering in restriction columns.
// Collections rename in place (double-click or F2). The search takes the
// OutlinerFilter.hpp syntax. Every edit runs an operator. In Play and
// Simulate it lists the running scene: rows still select, operators that
// allow it change the running scene only, and an entity's menu offers Apply
// to Edit Scene (play.apply_to_edit).
//
// The rows are cached (OutlinerRowCache): the tree is rebuilt only when the
// edit history, the document, the entity count, the collections, the
// search, or the display mode change (and every half second or so while the
// running scene, which game code edits without history, is shown), and only
// the rows in view are drawn (ImGuiListClipper). When the active object
// changes elsewhere (a viewport pick) its first row is opened and scrolled
// into view.
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
    // Entity rows shown (open, whether or not scrolled into view) as of the
    // last frame, in display order; Shift click ranges over them.
    const std::vector<SceneEntityId>& displayedEntities() const;
    // How often the rows were rebuilt, for tests and benchmarks.
    const OutlinerRowCache::Stats& rowCacheStats() const;
    // The collection being renamed in place, if any.
    std::optional<SceneCollectionId> renamingCollection() const;

    // ImGui ids of rows, so tools and tests can address them. windowId is
    // the id of the window the panel draws in; scope is OutlinerNode::scope.
    static ImGuiID entityRowId(ImGuiID windowId, SceneEntityId entity, uint64_t scope = 0);
    static ImGuiID collectionRowId(ImGuiID windowId, SceneCollectionId collection);
    // A row's restriction toggle: "visible", "selectable", or "renderable".
    static ImGuiID restrictionToggleId(ImGuiID rowId, const char* column);

  private:
    // Where a dragged row lands relative to the row under the mouse.
    enum class DropPlacement : uint8_t {
        Onto,
        Before,
        After,
    };

    // A row between whose siblings drops reorder: its entity and parent.
    struct ReorderTarget {
        SceneEntityId entity;
        SceneEntityId parent;
    };

    void drawHeader(EditorContext& context);
    void drawFilterOptions(EditorContext& context);
    // Brings the cached rows up to date with the scene and the panel state.
    void updateRows(EditorContext& context);
    // Opens and scrolls to the active object when something else made it so.
    void revealActive(const EditorContext& context);
    void drawRows(EditorContext& context);
    void drawRow(EditorContext& context, uint32_t entryIndex);
    void drawEntity(EditorContext& context, const OutlinerRowEntry& entry);
    void drawCollection(EditorContext& context, const OutlinerRowEntry& entry);
    void drawCollectionRename(EditorContext& context, ImVec2 rowMin, ImVec2 rowMax);
    void drawGroup(const OutlinerRowEntry& entry);
    void drawAsset(const OutlinerRowEntry& entry);
    // The dotted guides joining the row to its ancestors.
    void drawGuides(const OutlinerRowEntry& entry) const;
    // Draws a row's tree node; returns whether it is open. Records the row's
    // rectangle (m_rowMin, m_rowMax).
    bool treeRow(const OutlinerRowEntry& entry, ImGuiTreeNodeFlags flags);
    bool isOpen(const OutlinerRowEntry& entry) const;
    // Opens or closes every row with children (Expand All, Collapse All).
    void setAllOpen(bool open);

    void clickEntity(EditorContext& context, SceneEntityId entity);
    void drawEntityContextMenu(EditorContext& context, ImGuiID rowId, SceneEntityId entity);
    void drawCollectionContextMenu(EditorContext& context, ImGuiID rowId, SceneCollectionId collection);
    void drawExpandItems();
    void beginCollectionRename(EditorContext& context, ImGuiID rowId, SceneCollectionId collection);
    // Accepts a dragged row on the last item: parents the dragged objects to
    // parent (none for the scene root), links them to collection when it is
    // set, or, between two rows of reorder's siblings, moves the dragged
    // object there.
    void acceptEntityDrop(EditorContext& context, SceneEntityId parent, SceneCollectionId collection = {},
                          const std::optional<ReorderTarget>& reorder = std::nullopt);
    void dropBetween(EditorContext& context, SceneEntityId dragged, const ReorderTarget& reorder,
                     DropPlacement placement);
    // Orphan Data's unused assets, refreshed when the document or the
    // project's assets change.
    std::vector<OrphanAsset> orphanAssets(EditorContext& context);

    const ProjectAssets* m_projectAssets{nullptr};
    OutlinerDisplayMode m_mode{OutlinerDisplayMode::ViewLayer};
    std::string m_filter;
    bool m_preserveWorldOnReparent{true};

    OutlinerRowCache m_rows;
    // Rows the user opened or closed, by row id; others use their kind's
    // default (collections and groups open, entities closed). Searching
    // opens every row leading to a match without changing these.
    std::unordered_map<ImGuiID, bool> m_openRows;
    // Changes whenever a row opens or closes, so the visible rows follow.
    uint64_t m_openRevision{0};
    uint64_t m_orphanGeneration{0};
    uint64_t m_frames{0};

    // Per frame: the id the panel's rows hash from, where the rows start
    // (screen space, and window-local y), and the rectangle of the row drawn
    // last.
    ImGuiID m_seed{0};
    ImVec2 m_rowsOrigin{};
    float m_rowsLocalY{0.0f};
    ImVec2 m_rowMin{};
    ImVec2 m_rowMax{};
    // The height of a row, measured by the clipper (0 until drawn once).
    float m_rowHeight{0.0f};
    std::vector<SceneEntityId> m_lastDisplayed;
    // The row a Shift click ranges from: the last row clicked without Shift.
    std::optional<SceneEntityId> m_rangeAnchor;
    // The row (by id, since an entity may have several) of a selected entity
    // pressed without modifiers; it selects the entity alone on release
    // unless the press became a drag.
    std::optional<ImGuiID> m_pendingPlainClick;
    // The scene content generation the two above belong to.
    uint64_t m_sceneGeneration{0};
    // The row whose context menu is open, kept drawn while it is scrolled
    // away; the one this frame's rows found open.
    std::optional<ImGuiID> m_contextRow;
    std::optional<ImGuiID> m_contextRowThisFrame;
    // The active object as of the end of the last frame, and the row to
    // scroll into view once rows have a height.
    std::optional<SceneEntityId> m_knownActive;
    std::optional<ImGuiID> m_revealRow;

    // The collection row the mouse was over last frame (F2 renames it), and
    // the in-place rename.
    struct HoveredCollection {
        ImGuiID row{0};
        SceneCollectionId collection;
    };

    std::optional<HoveredCollection> m_hoveredCollection;

    struct CollectionRename {
        ImGuiID row{0};
        SceneCollectionId collection;
        std::string name;
        // Frames drawn without the text field active; the rename ends when
        // focus never arrives.
        int idleFrames{0};
        bool wasActive{false};
    };

    std::optional<CollectionRename> m_rename;

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
