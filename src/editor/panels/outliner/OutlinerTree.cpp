#include "OutlinerTree.hpp"

#include "OutlinerFilter.hpp"

#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace {
// Prefab group scopes have the top bit set so they never equal a collection id.
constexpr uint64_t prefabScopeBit = uint64_t{1} << 63u;

bool memberOf(const Scene& scene, Entity entity, SceneCollectionId collection) {
    const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity);
    return membership != nullptr && membership->contains(collection);
}

bool inAnyCollection(const Scene& scene, Entity entity) {
    const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity);
    return membership != nullptr && !membership->collections.empty();
}

Entity parentOf(const Scene& scene, Entity entity) {
    return scene.registry().get<RelationshipComponent>(entity).parent;
}

// Every entity with a stable id and a place in the hierarchy, in id order.
std::vector<Entity> entitiesById(const Scene& scene) {
    std::vector<std::pair<SceneEntityId, Entity>> entities;
    for (const Entity entity : scene.registry().view<const SceneEntityIdComponent, const RelationshipComponent>()) {
        entities.emplace_back(scene.id(entity), entity);
    }
    std::ranges::sort(entities, {}, &std::pair<SceneEntityId, Entity>::first);
    std::vector<Entity> sorted;
    sorted.reserve(entities.size());
    for (const auto& [id, entity] : entities) {
        sorted.push_back(entity);
    }
    return sorted;
}

OutlinerNode entityLeaf(const Scene& scene, Entity entity, uint64_t scope) {
    return {.kind = OutlinerNodeKind::Entity,
            .label = outlinerEntityLabel(scene, entity),
            .entity = scene.id(entity),
            .scope = scope,
            .transient = scene.isTransient(entity)};
}

// entity and its descendants, in the scene's child order.
OutlinerNode entityTree(const Scene& scene, Entity entity, uint64_t scope) {
    OutlinerNode node = entityLeaf(scene, entity, scope);
    for (const Entity child : scene.registry().get<RelationshipComponent>(entity).children) {
        if (scene.registry().valid(child)) {
            node.children.push_back(entityTree(scene, child, scope));
        }
    }
    return node;
}

OutlinerNode collectionNode(const SceneCollection& collection) {
    return {.kind = OutlinerNodeKind::Collection, .label = collection.name, .collection = collection.id};
}

// The collection, its nested collections, then its members: as hierarchies
// rooted at members whose parent is not a member too (ViewLayer), or as a
// flat list (Collections).
OutlinerNode collectionTree(const Scene& scene, const SceneCollection& collection, const std::vector<Entity>& entities,
                            OutlinerDisplayMode mode) {
    OutlinerNode node = collectionNode(collection);
    for (const SceneCollection& nested : scene.collections().all()) {
        if (nested.parent == collection.id) {
            node.children.push_back(collectionTree(scene, nested, entities, mode));
        }
    }
    const uint64_t scope = collection.id.value;
    for (const Entity entity : entities) {
        if (!memberOf(scene, entity, collection.id)) {
            continue;
        }
        if (mode == OutlinerDisplayMode::Collections) {
            node.children.push_back(entityLeaf(scene, entity, scope));
            continue;
        }
        const Entity parent = parentOf(scene, entity);
        if (parent == entt::null || !memberOf(scene, parent, collection.id)) {
            node.children.push_back(entityTree(scene, entity, scope));
        }
    }
    return node;
}

std::vector<OutlinerNode> collectionRows(const Scene& scene, OutlinerDisplayMode mode) {
    const std::vector<Entity> entities = entitiesById(scene);
    std::vector<OutlinerNode> rows;
    for (const SceneCollection& collection : scene.collections().all()) {
        if (!collection.parent) {
            rows.push_back(collectionTree(scene, collection, entities, mode));
        }
    }
    for (const Entity entity : entities) {
        if (inAnyCollection(scene, entity)) {
            continue;
        }
        if (mode == OutlinerDisplayMode::Collections) {
            rows.push_back(entityLeaf(scene, entity, 0));
        } else if (parentOf(scene, entity) == entt::null) {
            rows.push_back(entityTree(scene, entity, 0));
        }
    }
    return rows;
}

std::vector<OutlinerNode> prefabRows(const Scene& scene) {
    // Ordered by path, so groups sort and each gets a stable scope.
    std::map<std::string, std::vector<Entity>> instances;
    for (const Entity entity : entitiesById(scene)) {
        if (const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(entity)) {
            instances[instance->prefab].push_back(entity);
        }
    }
    std::vector<OutlinerNode> rows;
    uint64_t group = 0;
    for (const auto& [path, roots] : instances) {
        OutlinerNode node{.kind = OutlinerNodeKind::Prefab, .label = path, .path = path};
        const uint64_t scope = prefabScopeBit | ++group;
        for (const Entity root : roots) {
            node.children.push_back(entityTree(scene, root, scope));
        }
        rows.push_back(std::move(node));
    }
    return rows;
}

std::vector<OutlinerNode> orphanRows(const Scene& scene, std::span<const OrphanAsset> assets) {
    std::vector<OutlinerNode> rows;
    if (!assets.empty()) {
        OutlinerNode group{.kind = OutlinerNodeKind::Group, .label = "Unused Assets"};
        for (const OrphanAsset& asset : assets) {
            group.children.push_back(
                {.kind = OutlinerNodeKind::Asset, .label = asset.path, .path = asset.path, .assetKind = asset.kind});
        }
        rows.push_back(std::move(group));
    }
    const std::vector<SceneCollectionId> empty = findEmptyCollections(scene);
    if (!empty.empty()) {
        OutlinerNode group{.kind = OutlinerNodeKind::Group, .label = "Empty Collections"};
        for (const SceneCollectionId id : empty) {
            group.children.push_back(collectionNode(*scene.collections().find(id)));
        }
        rows.push_back(std::move(group));
    }
    return rows;
}

// Keeps rows that match or have a kept row below them.
void applyFilter(const Scene& scene, std::vector<OutlinerNode>& rows, const OutlinerFilterMatcher& matcher) {
    std::erase_if(rows, [&](OutlinerNode& row) {
        applyFilter(scene, row.children, matcher);
        bool matches = false;
        if (row.kind == OutlinerNodeKind::Entity) {
            const Entity entity = scene.findEntity(row.entity);
            matches = entity != entt::null && matcher.matchesEntity(scene, entity, row.label);
        } else {
            matches = matcher.matchesLabel(row.label);
        }
        row.filterContext = !matches;
        return !matches && row.children.empty();
    });
}
} // namespace

std::string_view outlinerDisplayModeKey(OutlinerDisplayMode mode) {
    switch (mode) {
    case OutlinerDisplayMode::Collections:
        return "collections";
    case OutlinerDisplayMode::Prefabs:
        return "prefabs";
    case OutlinerDisplayMode::OrphanData:
        return "orphan_data";
    case OutlinerDisplayMode::ViewLayer:
        break;
    }
    return "view_layer";
}

std::string_view outlinerDisplayModeLabel(OutlinerDisplayMode mode) {
    switch (mode) {
    case OutlinerDisplayMode::Collections:
        return "Collections";
    case OutlinerDisplayMode::Prefabs:
        return "Prefabs";
    case OutlinerDisplayMode::OrphanData:
        return "Orphan Data";
    case OutlinerDisplayMode::ViewLayer:
        break;
    }
    return "View Layer";
}

std::optional<OutlinerDisplayMode> parseOutlinerDisplayMode(std::string_view key) {
    for (const OutlinerDisplayMode mode : outlinerDisplayModes) {
        if (outlinerDisplayModeKey(mode) == key) {
            return mode;
        }
    }
    return std::nullopt;
}

bool outlinerFilterMatches(std::string_view label, std::string_view filter) {
    return containsIgnoringCase(label, filter);
}

std::vector<OutlinerNode> buildOutlinerTree(const Scene& scene, const OutlinerTreeOptions& options) {
    std::vector<OutlinerNode> rows;
    switch (options.mode) {
    case OutlinerDisplayMode::ViewLayer:
    case OutlinerDisplayMode::Collections:
        rows = collectionRows(scene, options.mode);
        break;
    case OutlinerDisplayMode::Prefabs:
        rows = prefabRows(scene);
        break;
    case OutlinerDisplayMode::OrphanData:
        rows = orphanRows(scene, options.orphanAssets);
        break;
    }
    if (const OutlinerFilter filter = parseOutlinerFilter(options.filter); !filter.empty()) {
        const ComponentTypeRegistry& types =
            options.componentTypes != nullptr ? *options.componentTypes : *builtInComponentTypes();
        applyFilter(scene, rows, OutlinerFilterMatcher(filter, types));
    }
    return rows;
}
