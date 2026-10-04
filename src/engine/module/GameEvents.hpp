#pragma once

#include "engine/scene/SceneEntityId.hpp"

#include <string_view>

// Events the engine publishes to a gameplay session's EventQueue. Entities
// are named by scene id: an event outlives the frame it was published in, and
// its entities may be gone by the time it is read.

// A body began overlapping a sensor (a rigid body marked as a sensor).
struct TriggerEntered {
    static constexpr std::string_view eventKey = "zenith.trigger_entered";
    SceneEntityId sensor;
    SceneEntityId other;
};

// A body stopped overlapping a sensor, including because either was destroyed.
struct TriggerExited {
    static constexpr std::string_view eventKey = "zenith.trigger_exited";
    SceneEntityId sensor;
    SceneEntityId other;
};

// EntityCommandBuffer created an entity (the root of a spawned prefab).
struct EntitySpawned {
    static constexpr std::string_view eventKey = "zenith.entity_spawned";
    SceneEntityId entity;
};

// EntityCommandBuffer destroyed an entity and, with it, its descendants.
struct EntityDestroyed {
    static constexpr std::string_view eventKey = "zenith.entity_destroyed";
    SceneEntityId entity;
};

// The gameplay session's scene finished loading.
struct SceneLoaded {
    static constexpr std::string_view eventKey = "zenith.scene_loaded";
};
