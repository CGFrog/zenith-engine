#pragma once

#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <span>

// An authored pickup. Value is the amount added to a collector when their
// physics bodies begin touching.
struct CollectibleItemComponent {
    uint32_t value{1};
};

// Marks an entity as able to collect items. The counter is runtime state and
// is deliberately omitted from the scene document component description.
struct ItemCollectorComponent {
    uint32_t collectedValue{0};
};

struct ItemCollectionProgress {
    uint64_t collected{0};
    uint64_t total{0};
};

class ItemCollectionSystem {
  public:
    void fixedUpdate(Scene& scene, std::span<const ContactEvent> contacts) const;
};

ItemCollectionProgress itemCollectionProgress(const Scene& scene);
