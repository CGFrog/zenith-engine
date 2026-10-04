#pragma once

#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

class GameServices;
class InputActionState;
class Logger;
class PhysicsWorld;

// What a Behaviour hook may touch. services is null in Edit mode (for
// behaviours that opt into it) and actions is null whenever no player input
// reaches the scene, as for systems (see SystemContext). physics is the
// scene's physics world, for raycasts and other queries; null where no
// application runs the hooks (Edit mode, onDestroy). deltaSeconds is the
// frame duration, or the fixed step in onFixedUpdate and the trigger and
// collision hooks; zero in onDestroy.
struct BehaviourContext {
    Scene& scene;
    Entity entity{entt::null};
    float deltaSeconds{0.0f};
    GameServices* services{nullptr};
    const InputActionState* actions{nullptr};
    Logger* logger{nullptr};
    PhysicsWorld* physics{nullptr};
};

// A body this behaviour's entity began touching. normal points from the
// behaviour's entity toward the other body.
struct BehaviourCollision {
    Entity other{entt::null};
    SceneEntityId otherId;
    glm::vec3 normal{0.0f};
};

// Per-entity game logic: derive from Behaviour, reflect the fields designers
// tune, and register the type with ModuleRegistrar::addBehaviour. The
// behaviour is then an ordinary component type (serialized, inspected,
// undoable, overridable in prefabs), and the engine calls its hooks in a
// deterministic order: by phase, then by registration order of the types,
// then by entity creation order (scene id).
//
// Lifecycle, checked before each phase's hooks:
// - onCreate once, the first time the engine sees the behaviour.
// - onEnable and onDisable when `enabled` changes (onEnable right after
//   onCreate when it starts enabled).
// - onStart once, before the first update hook of an enabled behaviour.
// - onDestroy when its entity is destroyed through the session's
//   EntityCommandBuffer (after onDisable when enabled). A scene destroyed
//   another way, or a session that stops, calls no hook.
//
// Disabled behaviours get no update, trigger, or collision hooks. Trigger and
// collision hooks run at the start of the next FixedUpdate after the physics
// step that produced them, before onFixedUpdate. Change the scene's entities
// from a hook through GameServices::commands(), never directly.
//
// A copy (a duplicated entity, a prefab instance, a Play session's copy of
// the edited scene) is a new behaviour that has not been created or started;
// assigning one behaviour to another keeps the target's lifecycle state.
class Behaviour {
  public:
    // Serialized as the "enabled" field of every behaviour.
    bool enabled{true};

    Behaviour() = default;

    Behaviour(const Behaviour& other)
        : enabled(other.enabled) {}

    Behaviour& operator=(const Behaviour& other) {
        enabled = other.enabled;
        return *this;
    }

    // Moves keep the lifecycle state: the ECS moves components as it packs
    // its storage.
    Behaviour(Behaviour&&) noexcept = default;
    Behaviour& operator=(Behaviour&&) noexcept = default;
    virtual ~Behaviour() = default;

    virtual void onCreate(BehaviourContext&) {}

    virtual void onStart(BehaviourContext&) {}

    virtual void onUpdate(BehaviourContext&) {}

    virtual void onFixedUpdate(BehaviourContext&) {}

    virtual void onLateUpdate(BehaviourContext&) {}

    // other entered or left this entity's sensor, or this entity entered or
    // left other's sensor.
    virtual void onTriggerEnter(BehaviourContext&, Entity /*other*/) {}

    virtual void onTriggerExit(BehaviourContext&, Entity /*other*/) {}

    virtual void onCollision(BehaviourContext&, const BehaviourCollision&) {}

    virtual void onEnable(BehaviourContext&) {}

    virtual void onDisable(BehaviourContext&) {}

    virtual void onDestroy(BehaviourContext&) {}

    bool created() const {
        return m_created;
    }

    bool started() const {
        return m_started;
    }

  private:
    friend class BehaviourLifecycle;

    bool m_created{false};
    bool m_started{false};
    // Whether onEnable ran more recently than onDisable.
    bool m_active{false};
};
