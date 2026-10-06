#pragma once

// Where a behaviour type was registered, for jumping to its source.

#include "Behaviour.hpp"
#include "engine/module/SystemScheduler.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Where a behaviour type was registered, for jumping to its source.
struct BehaviourSourceLocation {
    std::string file;
    uint32_t line{0};
    std::string function;
};

// One registered behaviour type, type-erased so the engine can run it. The
// functions hold game module code (see GameModule's lifetime rules).
struct BehaviourType {
    // Its component type's document key.
    std::string key;
    // The modes it runs in; Simulate runs Play behaviours in the phases the
    // Simulate mask enables for the behaviour systems.
    RunIn runIn{RunIn::Play};
    BehaviourSourceLocation source;
    // Appends every entity that has the behaviour, in no particular order.
    std::function<void(const Scene&, std::vector<Entity>&)> collect;
    // The entity's behaviour, or null when it has none.
    std::function<Behaviour*(Scene&, Entity)> find;
};

// The behaviour types a gameplay registered, in registration order, which is
// the order their hooks run in. Plain data plus functions: copyable, so a host
// can start from its own and let a module add to a copy.
class BehaviourRegistry {
  public:
    // Rejects an empty or duplicate key and a type without functions.
    SystemRegistryResult add(BehaviourType type);
    const BehaviourType* find(std::string_view key) const;
    std::span<const BehaviourType> types() const;
    bool empty() const;

  private:
    std::vector<BehaviourType> m_types;
};
