#pragma once

// Builds the outliner's tree for each display mode (View Layer, Collections, Prefabs, Orphan Data).

#include "editor/panels/outliner/OrphanData.hpp"
#include "engine/scene/Collections.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ComponentTypeRegistry;
class Scene;

// What the outliner lists, as Blender's outliner display modes. The key is
// the HierarchyPanel's saved area state, so never rename one.
enum class OutlinerDisplayMode : uint8_t {
    // The scene hierarchy grouped by collection: each collection lists its
    // member entities with their children, and entities in no collection
    // follow at the top level.
    ViewLayer,
    // The collection tree with each collection's members as a flat list,
    // then every entity in no collection.
    Collections,
    // Prefab instances grouped under their prefab asset.
    Prefabs,
    // Project assets the scene does not reference, and empty collections.
    OrphanData,
};

inline constexpr OutlinerDisplayMode outlinerDisplayModes[] = {
    OutlinerDisplayMode::ViewLayer, OutlinerDisplayMode::Collections, OutlinerDisplayMode::Prefabs,
    OutlinerDisplayMode::OrphanData};

// Stable key ("view_layer", "collections", "prefabs", "orphan_data").
std::string_view outlinerDisplayModeKey(OutlinerDisplayMode mode);
std::string_view outlinerDisplayModeLabel(OutlinerDisplayMode mode);
std::optional<OutlinerDisplayMode> parseOutlinerDisplayMode(std::string_view key);

enum class OutlinerNodeKind : uint8_t {
    Entity,
    Collection,
    // A prefab asset grouping its instances (Prefabs mode).
    Prefab,
    // A labelled group of rows (Orphan Data sections).
    Group,
    // A project asset (Orphan Data).
    Asset,
};

// One row of the outliner tree.
struct OutlinerNode {
    OutlinerNodeKind kind{OutlinerNodeKind::Entity};
    std::string label;
    // Entity rows.
    SceneEntityId entity;
    // Collection rows: the collection.
    SceneCollectionId collection;
    // Entity rows: tells apart rows of one entity listed in several places
    // (under each of its collections, or under each prefab group). Zero for
    // rows at the top level. Unique among the rows of one tree that show the
    // same entity.
    uint64_t scope{0};
    // Entity rows: generated content (Scene::isTransient), which is never
    // saved; the row is dimmed and says so in its tooltip.
    bool transient{false};
    // Prefab rows: the prefab's project-relative path. Asset rows: the
    // asset's path.
    std::string path;
    // Asset rows.
    EditorAssetKind assetKind{EditorAssetKind::Model};
    // The row does not match the filter and is listed only because a
    // descendant does.
    bool filterContext{false};
    std::vector<OutlinerNode> children;
};

struct OutlinerTreeOptions {
    OutlinerDisplayMode mode{OutlinerDisplayMode::ViewLayer};
    // The search (OutlinerFilter.hpp syntax: names, c:, t:, p:); empty lists
    // everything.
    std::string_view filter;
    // The component types c: terms name; null uses the engine's built-ins.
    const ComponentTypeRegistry* componentTypes{nullptr};
    // Orphan Data only: the assets to list (see findOrphanAssets).
    std::span<const OrphanAsset> orphanAssets;
};

// Whether label contains filter as plain text, ignoring ASCII case. An empty
// filter matches everything.
bool outlinerFilterMatches(std::string_view label, std::string_view filter);

// The rows of one display mode. Top-level entities, collection members, and
// instances are listed in ascending stable id order; children keep the
// scene's child order (RelationshipComponent::children, which documents
// save). With a filter, a row is kept when it matches every term or when a
// row below it does (then marked filterContext), so matches always appear
// with their ancestors. Rows that are not entities match name terms only.
std::vector<OutlinerNode> buildOutlinerTree(const Scene& scene, const OutlinerTreeOptions& options);
