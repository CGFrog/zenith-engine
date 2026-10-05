#include "OutlinerRows.hpp"

#include <imgui_internal.h>

#include <unordered_set>
#include <utility>

namespace {
void indexNodes(std::span<const OutlinerNode> nodes, ImGuiID seed, uint32_t depth, uint32_t parent,
                std::unordered_set<ImGuiID>& used, std::vector<OutlinerRowEntry>& entries) {
    for (const OutlinerNode& node : nodes) {
        ImGuiID id = outlinerRowBaseId(seed, node);
        for (uint32_t repeat = 1; !used.insert(id).second; ++repeat) {
            id = ImHashData(&repeat, sizeof(repeat), id);
        }
        const auto index = static_cast<uint32_t>(entries.size());
        entries.push_back({.node = &node, .id = id, .depth = depth, .parent = parent});
        indexNodes(node.children, seed, depth + 1, index, used, entries);
        entries[index].subtreeEnd = static_cast<uint32_t>(entries.size());
    }
}
} // namespace

ImGuiID outlinerEntityRowId(ImGuiID seed, SceneEntityId entity, uint64_t scope) {
    const ImGuiID scoped = scope == 0 ? seed : ImHashData(&scope, sizeof(scope), seed);
    const void* pointer = reinterpret_cast<const void*>(static_cast<uintptr_t>(entity.value));
    return ImHashData(&pointer, sizeof(pointer), scoped);
}

ImGuiID outlinerCollectionRowId(ImGuiID seed, SceneCollectionId collection) {
    return ImHashData(&collection.value, sizeof(collection.value), ImHashStr("collection", 0, seed));
}

ImGuiID outlinerRowBaseId(ImGuiID seed, const OutlinerNode& node) {
    switch (node.kind) {
    case OutlinerNodeKind::Entity:
        return outlinerEntityRowId(seed, node.entity, node.scope);
    case OutlinerNodeKind::Collection:
        return outlinerCollectionRowId(seed, node.collection);
    case OutlinerNodeKind::Prefab:
        return ImHashStr(node.label.c_str(), 0, ImHashStr("prefab", 0, seed));
    case OutlinerNodeKind::Group:
        return ImHashStr(node.label.c_str(), 0, ImHashStr("group", 0, seed));
    case OutlinerNodeKind::Asset:
        break;
    }
    return ImHashStr(node.path.c_str(), 0, ImHashStr("asset", 0, seed));
}

std::vector<OutlinerRowEntry> indexOutlinerRows(std::span<const OutlinerNode> rows, ImGuiID seed) {
    std::vector<OutlinerRowEntry> entries;
    std::unordered_set<ImGuiID> used;
    indexNodes(rows, seed, 0, OutlinerRowEntry::noParent, used, entries);
    return entries;
}

std::vector<uint32_t> visibleOutlinerRows(std::span<const OutlinerRowEntry> entries,
                                          const std::function<bool(const OutlinerRowEntry&)>& isOpen) {
    std::vector<uint32_t> visible;
    visible.reserve(entries.size());
    for (uint32_t index = 0; index < entries.size();) {
        const OutlinerRowEntry& entry = entries[index];
        visible.push_back(index);
        const bool hasChildren = entry.subtreeEnd > index + 1;
        index = hasChildren && !isOpen(entry) ? entry.subtreeEnd : index + 1;
    }
    return visible;
}

bool OutlinerRowCache::update(const OutlinerRowSource& source, const SceneCollections& collections,
                              const std::function<std::vector<OutlinerNode>()>& build) {
    if (m_source == source && m_collections == collections) {
        return false;
    }
    m_source = source;
    m_collections = collections;
    m_tree = build();
    m_entries = indexOutlinerRows(m_tree, source.seed);
    m_openRevision.reset();
    ++m_stats.treeBuilds;
    return true;
}

bool OutlinerRowCache::updateVisible(uint64_t openRevision,
                                     const std::function<bool(const OutlinerRowEntry&)>& isOpen) {
    if (m_openRevision == openRevision) {
        return false;
    }
    m_openRevision = openRevision;
    m_visible = visibleOutlinerRows(m_entries, isOpen);
    m_visibleById.clear();
    m_visibleById.reserve(m_visible.size());
    for (uint32_t position = 0; position < m_visible.size(); ++position) {
        m_visibleById.emplace(m_entries[m_visible[position]].id, position);
    }
    ++m_stats.visibleBuilds;
    return true;
}

void OutlinerRowCache::invalidate() {
    m_source.reset();
}

const std::vector<OutlinerNode>& OutlinerRowCache::tree() const {
    return m_tree;
}

const std::vector<OutlinerRowEntry>& OutlinerRowCache::entries() const {
    return m_entries;
}

const std::vector<uint32_t>& OutlinerRowCache::visible() const {
    return m_visible;
}

std::optional<uint32_t> OutlinerRowCache::visibleIndex(ImGuiID id) const {
    const auto found = m_visibleById.find(id);
    return found == m_visibleById.end() ? std::nullopt : std::optional<uint32_t>(found->second);
}

std::optional<uint32_t> OutlinerRowCache::findEntity(SceneEntityId entity) const {
    for (uint32_t index = 0; index < m_entries.size(); ++index) {
        const OutlinerNode& node = *m_entries[index].node;
        if (node.kind == OutlinerNodeKind::Entity && node.entity == entity) {
            return index;
        }
    }
    return std::nullopt;
}

const OutlinerRowCache::Stats& OutlinerRowCache::stats() const {
    return m_stats;
}
