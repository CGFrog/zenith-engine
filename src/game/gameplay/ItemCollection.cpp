#include "ItemCollection.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <vector>

namespace {
struct CollectionPair {
    SceneEntityId collector;
    SceneEntityId item;
};

bool pairLess(const CollectionPair& left, const CollectionPair& right) {
    return std::tie(left.item.value, left.collector.value) < std::tie(right.item.value, right.collector.value);
}

void appendPair(const Scene& scene, Entity collector, Entity item, std::vector<CollectionPair>& pairs) {
    const entt::registry& registry = scene.registry();
    if (!registry.valid(collector) || !registry.valid(item) || !registry.all_of<ItemCollectorComponent>(collector) ||
        !registry.all_of<CollectibleItemComponent>(item)) {
        return;
    }
    pairs.push_back({scene.id(collector), scene.id(item)});
}
} // namespace

void ItemCollectionSystem::fixedUpdate(Scene& scene, std::span<const ContactEvent> contacts) const {
    std::vector<CollectionPair> pairs;
    pairs.reserve(contacts.size());
    for (const ContactEvent& contact : contacts) {
        if (contact.type != ContactEventType::Begin) {
            continue;
        }
        appendPair(scene, contact.first, contact.second, pairs);
        appendPair(scene, contact.second, contact.first, pairs);
    }

    std::ranges::sort(pairs, pairLess);
    SceneEntityId previousItem;
    for (const CollectionPair& pair : pairs) {
        if (pair.item == previousItem) {
            continue;
        }
        previousItem = pair.item;

        const Entity collector = scene.findEntity(pair.collector);
        const Entity item = scene.findEntity(pair.item);
        entt::registry& registry = scene.registry();
        if (collector == entt::null || item == entt::null || !registry.valid(collector) || !registry.valid(item) ||
            !registry.all_of<ItemCollectorComponent>(collector) || !registry.all_of<CollectibleItemComponent>(item)) {
            continue;
        }

        auto& collectorState = registry.get<ItemCollectorComponent>(collector);
        const uint32_t value = registry.get<CollectibleItemComponent>(item).value;
        collectorState.collectedValue = value > (std::numeric_limits<uint32_t>::max)() - collectorState.collectedValue
                                            ? (std::numeric_limits<uint32_t>::max)()
                                            : collectorState.collectedValue + value;
        scene.destroyEntity(item);
    }
}

ItemCollectionProgress itemCollectionProgress(const Scene& scene) {
    ItemCollectionProgress progress;
    const entt::registry& registry = scene.registry();
    for (const Entity entity : registry.view<const ItemCollectorComponent>()) {
        progress.collected += registry.get<const ItemCollectorComponent>(entity).collectedValue;
    }
    progress.total = progress.collected;
    for (const Entity entity : registry.view<const CollectibleItemComponent>()) {
        progress.total += registry.get<const CollectibleItemComponent>(entity).value;
    }
    return progress;
}
