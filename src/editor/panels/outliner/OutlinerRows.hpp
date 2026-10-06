#pragma once

// The outliner's rows as a cached flat list, so the panel draws only the rows in view.

#include "editor/panels/outliner/OutlinerTree.hpp"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The outliner's rows as a flat list, so the panel draws only the rows in
// view (ImGuiListClipper) and rebuilds nothing while the scene and the
// outliner's state are unchanged.

// One node of an outliner tree in depth-first display order.
struct OutlinerRowEntry {
    static constexpr uint32_t noParent = std::numeric_limits<uint32_t>::max();

    const OutlinerNode* node{nullptr};
    // The row's ImGui id, unique among the tree's rows (see indexOutlinerRows).
    ImGuiID id{0};
    // Nesting level; top-level rows are 0.
    uint32_t depth{0};
    // The entry index just past this node's subtree.
    uint32_t subtreeEnd{0};
    // The entry index of the row this one is listed under, or noParent.
    uint32_t parent{noParent};
};

// Row ids, hashed from seed (the id of the window the rows are drawn in).
// An entity's scope is OutlinerNode::scope; scope zero hashes exactly as
// ImGui::TreeNodeEx(pointer id) in the window does.
ImGuiID outlinerEntityRowId(ImGuiID seed, SceneEntityId entity, uint64_t scope);
ImGuiID outlinerCollectionRowId(ImGuiID seed, SceneCollectionId collection);
// Any row: the id above for entities and collections, otherwise one hashed
// from the row's kind and label (groups, prefabs) or path (assets).
ImGuiID outlinerRowBaseId(ImGuiID seed, const OutlinerNode& node);

// Every node of rows depth first. A row whose base id an earlier row already
// has (an entity listed twice in one scope) hashes a repeat count onto it, so
// ids are unique and do not depend on which rows are open.
std::vector<OutlinerRowEntry> indexOutlinerRows(std::span<const OutlinerNode> rows, ImGuiID seed);

// The entry indices shown: every top-level entry and every entry whose
// ancestors are all open. Entries without children are never asked about.
std::vector<uint32_t> visibleOutlinerRows(std::span<const OutlinerRowEntry> entries,
                                          const std::function<bool(const OutlinerRowEntry&)>& isOpen);

// What an outliner's rows are built from, besides the scene's collection
// definitions. The tree is rebuilt only when one of these changes; anything
// else the rows show (selection, visibility flags, icons) is read while
// drawing.
struct OutlinerRowSource {
    OutlinerDisplayMode mode{OutlinerDisplayMode::ViewLayer};
    std::string filter;
    ImGuiID seed{0};
    // The edit history's revision: every recorded edit, undo, and redo.
    uint64_t historyRevision{0};
    // Scene::contentGeneration: the document was replaced.
    uint64_t contentGeneration{0};
    // Entities with a stable id: changes made outside the history (generated
    // content, the running game) usually add or remove some.
    size_t entityCount{0};
    // Changes when Orphan Data's asset list is recomputed.
    uint64_t orphanGeneration{0};
    // Changes periodically while the running scene is shown, which game code
    // edits without history.
    uint64_t refreshTick{0};

    bool operator==(const OutlinerRowSource&) const = default;
};

// The outliner tree, its entries, and the visible rows, each rebuilt only
// when what it derives from changes.
class OutlinerRowCache {
  public:
    struct Stats {
        // Times the tree (and its entries) was rebuilt.
        uint64_t treeBuilds{0};
        // Times the visible rows were recomputed.
        uint64_t visibleBuilds{0};
    };

    OutlinerRowCache() = default;
    // Entries point into the tree, which a move keeps in place and a copy
    // would not.
    OutlinerRowCache(const OutlinerRowCache&) = delete;
    OutlinerRowCache& operator=(const OutlinerRowCache&) = delete;
    OutlinerRowCache(OutlinerRowCache&&) = default;
    OutlinerRowCache& operator=(OutlinerRowCache&&) = default;

    // Rebuilds the tree with build when source or collections differ from
    // the last ones; returns whether it did.
    bool update(const OutlinerRowSource& source, const SceneCollections& collections,
                const std::function<std::vector<OutlinerNode>()>& build);
    // Recomputes the visible rows when the tree was rebuilt or openRevision
    // (which the caller changes whenever a row opens or closes) differs.
    // Returns whether it did.
    bool updateVisible(uint64_t openRevision, const std::function<bool(const OutlinerRowEntry&)>& isOpen);
    // Forgets the source, so the next update rebuilds.
    void invalidate();

    const std::vector<OutlinerNode>& tree() const;
    const std::vector<OutlinerRowEntry>& entries() const;
    // Entry indices in display order.
    const std::vector<uint32_t>& visible() const;
    // The visible position of the row with id, if it is visible.
    std::optional<uint32_t> visibleIndex(ImGuiID id) const;
    // The first entry (in display order) showing entity, if any.
    std::optional<uint32_t> findEntity(SceneEntityId entity) const;
    const Stats& stats() const;

  private:
    std::optional<OutlinerRowSource> m_source;
    SceneCollections m_collections;
    std::vector<OutlinerNode> m_tree;
    std::vector<OutlinerRowEntry> m_entries;
    std::vector<uint32_t> m_visible;
    std::unordered_map<ImGuiID, uint32_t> m_visibleById;
    std::optional<uint64_t> m_openRevision;
    Stats m_stats;
};
