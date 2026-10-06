#pragma once

// Blender-style collections: named, nested groups of entities that are independent of the transform hierarchy.

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Blender-style collections: named, nested groups of entities that are
// independent of the transform hierarchy. An entity may belong to any number
// of collections (CollectionMembershipComponent) or to none, which leaves it
// in the scene's implicit root collection. Collections carry the flags the
// editor and renderer filter by (see SceneVisibility.hpp) and an instance
// offset used when another entity instances them (CollectionInstanceComponent).

struct SceneCollectionId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const SceneCollectionId&) const = default;
};

enum class CollectionFlag : uint8_t {
    // Shown in editor viewports. Editor-only: game builds and Play mode ignore it.
    Visible,
    // Members can be picked in editor viewports.
    Selectable,
    // Members are drawn at all; false excludes them from the game too.
    Renderable,
};

struct SceneCollection {
    SceneCollectionId id;
    std::string name;
    // The enclosing collection; empty for a top-level collection.
    SceneCollectionId parent;
    bool visible{true};
    bool selectable{true};
    bool renderable{true};
    // Point of the members' space that lands on an instancing entity's origin
    // (world units, subtracted from the members' world positions).
    glm::vec3 instanceOffset{0.0f};

    bool flag(CollectionFlag which) const;
    void setFlag(CollectionFlag which, bool value);

    bool operator==(const SceneCollection&) const = default;
};

struct CollectionResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The collection definitions of one scene, kept in ascending id order. Ids are
// stable across save and load and never reused within a document's lifetime
// as long as the next id is preserved (it follows the largest id present).
class SceneCollections {
  public:
    // Creates a collection with a fresh id under parent (empty for top level).
    CollectionResult create(std::string name, SceneCollectionId parent, SceneCollectionId& created);
    // Adds a collection with a given id, as loading and undo do. Its parent
    // must already exist, so add parents first (ascending ids guarantee it for
    // collections this class created).
    CollectionResult add(SceneCollection collection);
    // Removes a collection; its child collections move to its parent.
    // Membership of entities is not changed here (see unlinkCollection).
    CollectionResult remove(SceneCollectionId id);
    CollectionResult rename(SceneCollectionId id, std::string name);
    CollectionResult setFlag(SceneCollectionId id, CollectionFlag flag, bool value);
    CollectionResult setInstanceOffset(SceneCollectionId id, glm::vec3 offset);
    // Moves a collection under another (or to the top level) and refuses cycles.
    CollectionResult setParent(SceneCollectionId id, SceneCollectionId parent);

    const SceneCollection* find(SceneCollectionId id) const;
    std::span<const SceneCollection> all() const;
    bool empty() const;
    // Whether the collection and every enclosing collection have the flag.
    // False for an unknown id.
    bool effectiveFlag(SceneCollectionId id, CollectionFlag flag) const;
    // id followed by every collection nested inside it, at any depth.
    std::vector<SceneCollectionId> subtree(SceneCollectionId id) const;

    bool operator==(const SceneCollections&) const = default;

  private:
    SceneCollection* findMutable(SceneCollectionId id);

    std::vector<SceneCollection> m_collections;
    uint64_t m_nextId{1};
};

// Collections an entity belongs to, sorted and without duplicates. Every id
// names a collection of the entity's scene; documents referencing a missing
// collection are rejected. Created by collection.link, not added by hand.
struct CollectionMembershipComponent {
    std::vector<SceneCollectionId> collections;

    bool contains(SceneCollectionId id) const;
    // Inserts in order; false when already present.
    bool insert(SceneCollectionId id);
    // False when absent.
    bool erase(SceneCollectionId id);
};

// Draws the members of a collection (and of the collections nested in it) at
// this entity's transform, offset by the collection's instanceOffset. The
// instanced entities are not copied into the scene; the renderer expands them
// each time it rebuilds its render items (see expandCollectionInstances).
struct CollectionInstanceComponent {
    SceneCollectionId collection;
};

// Per-entity visibility. `hidden` is the editor viewport hide (H / Alt+H): it
// is saved with the scene but ignored by games and Play mode. `selectable`
// only affects editor picking. `renderable` false removes the entity from
// every render, the game's included. Entities without the component use the
// defaults.
struct VisibilityComponent {
    bool hidden{false};
    bool selectable{true};
    bool renderable{true};

    bool operator==(const VisibilityComponent&) const = default;
};

class Scene;

// Scene-level collection edits. Each validates its ids, changes membership
// components and the scene's collection definitions together, and marks the
// scene's render state dirty so renderers rebuild their items.

// Adds entity to an existing collection; succeeds without change when it is
// already a member.
CollectionResult linkToCollection(Scene& scene, entt::entity entity, SceneCollectionId collection);
// Removes entity from a collection; succeeds without change when it is not a
// member. An emptied membership component is removed.
CollectionResult unlinkFromCollection(Scene& scene, entt::entity entity, SceneCollectionId collection);
// Deletes a collection the way Blender's outliner Delete does: its nested
// collections and its members move to its parent collection (members of a
// top-level collection return to the scene root), and entities instancing it
// lose their CollectionInstanceComponent.
CollectionResult deleteCollection(Scene& scene, SceneCollectionId collection);
// Empty when every membership and instance reference names an existing
// collection, otherwise a description of the first one that does not.
std::string findMissingCollectionReference(const Scene& scene, const SceneCollections& collections);
