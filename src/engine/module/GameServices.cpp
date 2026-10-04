#include "GameServices.hpp"

#include "GameEvents.hpp"
#include "engine/behaviour/BehaviourScheduler.hpp"

#include <utility>

GameServices::GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger,
                           std::shared_ptr<const BehaviourRegistry> behaviours)
    : m_prefabs(assets, std::move(documents))
    , m_debugDraw(debugDraw)
    , m_logger(logger)
    , m_log(logger, "gameplay")
    , m_behaviours(std::move(behaviours)) {}

PrefabSystem& GameServices::prefabs() {
    return m_prefabs;
}

EntityCommandBuffer& GameServices::commands() {
    return m_commands;
}

EventQueue& GameServices::events() {
    return m_events;
}

Timers& GameServices::timers() {
    return m_timers;
}

Effects& GameServices::effects() {
    return m_effects;
}

DebugDraw& GameServices::debugDraw() {
    return m_debugDraw;
}

const LogChannel& GameServices::log() const {
    return m_log;
}

void GameServices::beginFrame() {
    m_events.advanceFrame();
}

void GameServices::advanceTimers(Scene& scene, float simulatedSeconds) {
    m_timers.advance(scene, simulatedSeconds);
    flush(scene);
}

void GameServices::flush(Scene& scene) {
    m_effects.flush(scene);
    if (!m_behaviours) {
        m_commands.flush(scene, m_prefabs, m_events, m_log);
        return;
    }
    m_commands.flush(scene, m_prefabs, m_events, m_log, [this](Scene& destroyed, Entity entity) {
        destroyBehaviours(*m_behaviours, destroyed, entity, this, m_logger);
    });
}

void GameServices::publishPhysicsEvents(const Scene& scene, std::span<const TriggerEvent> triggers,
                                        std::span<const ContactEvent> contacts) {
    for (const TriggerEvent& event : triggers) {
        const bool entered = event.type == TriggerEventType::Entered;
        if (entered) {
            m_events.publish(TriggerEntered{.sensor = event.sensorId, .other = event.otherId});
        } else {
            m_events.publish(TriggerExited{.sensor = event.sensorId, .other = event.otherId});
        }
        m_physicsEvents.triggers.push_back({.entered = entered, .sensor = event.sensorId, .other = event.otherId});
    }
    const entt::registry& registry = scene.registry();
    const auto solid = [&registry](Entity entity) {
        const auto* body = registry.valid(entity) ? registry.try_get<RigidBodyComponent>(entity) : nullptr;
        return registry.valid(entity) && (body == nullptr || !body->sensor);
    };
    for (const ContactEvent& contact : contacts) {
        if (contact.type == ContactEventType::Begin && solid(contact.first) && solid(contact.second)) {
            m_physicsEvents.collisions.push_back(
                {.first = scene.id(contact.first), .second = scene.id(contact.second), .normal = contact.normal});
        }
    }
}

GameplayPhysicsEvents GameServices::takePhysicsEvents() {
    return std::exchange(m_physicsEvents, {});
}

void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile) {
    scheduler.run(phase, context, profile);
    if (context.services != nullptr) {
        context.services->flush(context.scene);
    }
}
