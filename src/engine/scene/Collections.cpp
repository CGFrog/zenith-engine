#include "Collections.hpp"

#include "Scene.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
std::string describe(SceneCollectionId id) {
    return "collection " + std::to_string(id.value);
}
} // namespace

bool SceneCollection::flag(CollectionFlag which) const {
    switch (which) {
    case CollectionFlag::Visible:
        return visible;
    case CollectionFlag::Selectable:
        return selectable;
    case CollectionFlag::Renderable:
        return renderable;
    }
    return false;
}

void SceneCollection::setFlag(CollectionFlag which, bool value) {
    switch (which) {
    case CollectionFlag::Visible:
        visible = value;
        break;
    case CollectionFlag::Selectable:
        selectable = value;
        break;
    case CollectionFlag::Renderable:
        renderable = value;
        break;
    }
}

CollectionResult SceneCollections::create(std::string name, SceneCollectionId parent, SceneCollectionId& created) {
    const SceneCollectionId id{m_nextId};
    if (CollectionResult result = add({.id = id, .name = std::move(name), .parent = parent}); !result) {
        return result;
    }
    created = id;
    return {.success = true};
}

CollectionResult SceneCollections::add(SceneCollection collection) {
    if (!collection.id) {
        return {.error = "Collection id 0 is reserved"};
    }
    if (find(collection.id) != nullptr) {
        return {.error = "Duplicate " + describe(collection.id)};
    }
    if (collection.parent && find(collection.parent) == nullptr) {
        return {.error = "Collection " + std::to_string(collection.id.value) + " references missing parent " +
                         describe(collection.parent)};
    }
    if (collection.parent == collection.id) {
        return {.error = "A collection cannot contain itself"};
    }
    const auto position = std::ranges::upper_bound(m_collections, collection.id, {}, &SceneCollection::id);
    const uint64_t id = collection.id.value;
    m_collections.insert(position, std::move(collection));
    m_nextId = id == std::numeric_limits<uint64_t>::max() ? id : std::max(m_nextId, id + 1);
    return {.success = true};
}

CollectionResult SceneCollections::remove(SceneCollectionId id) {
    const auto found = std::ranges::find(m_collections, id, &SceneCollection::id);
    if (found == m_collections.end()) {
        return {.error = "Unknown " + describe(id)};
    }
    const SceneCollectionId parent = found->parent;
    m_collections.erase(found);
    for (SceneCollection& collection : m_collections) {
        if (collection.parent == id) {
            collection.parent = parent;
        }
    }
    return {.success = true};
}

CollectionResult SceneCollections::rename(SceneCollectionId id, std::string name) {
    SceneCollection* collection = findMutable(id);
    if (collection == nullptr) {
        return {.error = "Unknown " + describe(id)};
    }
    collection->name = std::move(name);
    return {.success = true};
}

CollectionResult SceneCollections::setFlag(SceneCollectionId id, CollectionFlag flag, bool value) {
    SceneCollection* collection = findMutable(id);
    if (collection == nullptr) {
        return {.error = "Unknown " + describe(id)};
    }
    collection->setFlag(flag, value);
    return {.success = true};
}

CollectionResult SceneCollections::setInstanceOffset(SceneCollectionId id, glm::vec3 offset) {
    SceneCollection* collection = findMutable(id);
    if (collection == nullptr) {
        return {.error = "Unknown " + describe(id)};
    }
    collection->instanceOffset = offset;
    return {.success = true};
}

CollectionResult SceneCollections::setParent(SceneCollectionId id, SceneCollectionId parent) {
    SceneCollection* collection = findMutable(id);
    if (collection == nullptr) {
        return {.error = "Unknown " + describe(id)};
    }
    if (parent && find(parent) == nullptr) {
        return {.error = "Unknown parent " + describe(parent)};
    }
    for (SceneCollectionId ancestor = parent; ancestor;) {
        if (ancestor == id) {
            return {.error = "Moving " + describe(id) + " there would nest it inside itself"};
        }
        const SceneCollection* enclosing = find(ancestor);
        ancestor = enclosing == nullptr ? SceneCollectionId{} : enclosing->parent;
    }
    collection->parent = parent;
    return {.success = true};
}

const SceneCollection* SceneCollections::find(SceneCollectionId id) const {
    const auto found = std::ranges::lower_bound(m_collections, id, {}, &SceneCollection::id);
    return found != m_collections.end() && found->id == id ? &*found : nullptr;
}

SceneCollection* SceneCollections::findMutable(SceneCollectionId id) {
    return const_cast<SceneCollection*>(std::as_const(*this).find(id));
}

std::span<const SceneCollection> SceneCollections::all() const {
    return m_collections;
}

bool SceneCollections::empty() const {
    return m_collections.empty();
}

bool SceneCollections::effectiveFlag(SceneCollectionId id, CollectionFlag flag) const {
    const SceneCollection* collection = find(id);
    if (collection == nullptr) {
        return false;
    }
    // add() and setParent() keep the parent chain acyclic, and it is at most
    // as long as the collection list.
    for (size_t depth = 0; collection != nullptr && depth <= m_collections.size(); ++depth) {
        if (!collection->flag(flag)) {
            return false;
        }
        collection = collection->parent ? find(collection->parent) : nullptr;
    }
    return true;
}

std::vector<SceneCollectionId> SceneCollections::subtree(SceneCollectionId id) const {
    std::vector<SceneCollectionId> result;
    if (find(id) == nullptr) {
        return result;
    }
    result.push_back(id);
    // Breadth-first: each pass appends the children of the ids found so far.
    for (size_t index = 0; index < result.size(); ++index) {
        for (const SceneCollection& collection : m_collections) {
            if (collection.parent == result[index]) {
                result.push_back(collection.id);
            }
        }
    }
    return result;
}

bool CollectionMembershipComponent::contains(SceneCollectionId id) const {
    return std::ranges::binary_search(collections, id);
}

bool CollectionMembershipComponent::insert(SceneCollectionId id) {
    const auto position = std::ranges::lower_bound(collections, id);
    if (position != collections.end() && *position == id) {
        return false;
    }
    collections.insert(position, id);
    return true;
}

bool CollectionMembershipComponent::erase(SceneCollectionId id) {
    const auto position = std::ranges::lower_bound(collections, id);
    if (position == collections.end() || *position != id) {
        return false;
    }
    collections.erase(position);
    return true;
}

namespace {
void markMembershipDirty(Scene& scene, Entity entity) {
    scene.markRenderableDirty(entity);
    scene.markLightDirty(entity);
}
} // namespace

CollectionResult linkToCollection(Scene& scene, Entity entity, SceneCollectionId collection) {
    if (!scene.registry().valid(entity)) {
        return {.error = "Cannot link an invalid entity to a collection"};
    }
    if (scene.collections().find(collection) == nullptr) {
        return {.error = "Unknown " + describe(collection)};
    }
    if (scene.registry().get_or_emplace<CollectionMembershipComponent>(entity).insert(collection)) {
        markMembershipDirty(scene, entity);
    }
    return {.success = true};
}

CollectionResult unlinkFromCollection(Scene& scene, Entity entity, SceneCollectionId collection) {
    if (!scene.registry().valid(entity)) {
        return {.error = "Cannot unlink an invalid entity from a collection"};
    }
    auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity);
    if (membership == nullptr || !membership->erase(collection)) {
        return {.success = true};
    }
    if (membership->collections.empty()) {
        scene.registry().remove<CollectionMembershipComponent>(entity);
    }
    markMembershipDirty(scene, entity);
    return {.success = true};
}

CollectionResult deleteCollection(Scene& scene, SceneCollectionId collection) {
    const SceneCollection* removed = scene.collections().find(collection);
    if (removed == nullptr) {
        return {.error = "Unknown " + describe(collection)};
    }
    const SceneCollectionId parent = removed->parent;
    SceneCollections remaining = scene.collections();
    if (CollectionResult result = remaining.remove(collection); !result) {
        return result;
    }
    entt::registry& registry = scene.registry();
    std::vector<Entity> emptied;
    for (auto [entity, membership] : registry.view<CollectionMembershipComponent>().each()) {
        if (!membership.erase(collection)) {
            continue;
        }
        if (parent) {
            membership.insert(parent);
        }
        if (membership.collections.empty()) {
            emptied.push_back(entity);
        }
    }
    registry.remove<CollectionMembershipComponent>(emptied.begin(), emptied.end());
    std::vector<Entity> instancers;
    for (const auto [entity, instance] : registry.view<const CollectionInstanceComponent>().each()) {
        if (instance.collection == collection) {
            instancers.push_back(entity);
        }
    }
    registry.remove<CollectionInstanceComponent>(instancers.begin(), instancers.end());
    // Bumps the render and light revisions, covering the membership edits.
    scene.setCollections(std::move(remaining));
    return {.success = true};
}

std::string findMissingCollectionReference(const Scene& scene, const SceneCollections& collections) {
    const entt::registry& registry = scene.registry();
    for (const auto [entity, membership] : registry.view<const CollectionMembershipComponent>().each()) {
        for (const SceneCollectionId id : membership.collections) {
            if (collections.find(id) == nullptr) {
                return "Entity " + std::to_string(scene.id(entity).value) + " is a member of missing " + describe(id);
            }
        }
    }
    for (const auto [entity, instance] : registry.view<const CollectionInstanceComponent>().each()) {
        if (collections.find(instance.collection) == nullptr) {
            return "Entity " + std::to_string(scene.id(entity).value) + " instances missing " +
                   describe(instance.collection);
        }
    }
    return {};
}
