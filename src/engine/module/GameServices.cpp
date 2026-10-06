#include "GameServices.hpp"

#include "GameEvents.hpp"
#include "engine/application/Application.hpp"
#include "engine/behaviour/BehaviourScheduler.hpp"

#include <utility>

GameServices::GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger,
                           std::shared_ptr<const BehaviourRegistry> behaviours)
    : m_logger(logger)
    , m_log(logger, "gameplay")
    , m_behaviours(std::move(behaviours))
    , m_world(assets, documents, m_time.timers(), m_log)
    , m_outside(assets, documents, debugDraw, m_log)
    , m_content(documents.assetRoot, m_log) {}

WorldServices& GameServices::world() {
    return m_world;
}

TimeServices& GameServices::time() {
    return m_time;
}

const TimeServices& GameServices::time() const {
    return m_time;
}

ContentServices& GameServices::content() {
    return m_content;
}

OutsideServices& GameServices::outside() {
    return m_outside;
}

const OutsideServices& GameServices::outside() const {
    return m_outside;
}

EventQueue& GameServices::events() {
    return m_events;
}

const LogChannel& GameServices::log() const {
    return m_log;
}

LogChannel GameServices::log(std::string_view name) const {
    return m_log.withCategory("gameplay." + std::string(name));
}

void GameServices::beginFrame() {
    m_events.advanceFrame();
}

void GameServices::advanceTimers(Scene& scene, float simulatedSeconds) {
    m_time.timers().advance(scene, simulatedSeconds, m_log);
    flush(scene);
}

void GameServices::flush(Scene& scene) {
    m_world.effects().flush(scene);
    EntityCommandBuffer& commands = m_world.commands();
    if (!m_behaviours) {
        commands.flush(scene, m_world.prefabs(), m_events, m_log);
        return;
    }
    commands.flush(scene, m_world.prefabs(), m_events, m_log, [this](Scene& destroyed, Entity entity) {
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

void GameServices::requestAssetCollection() {
    m_assetCollectionRequested = true;
}

bool GameServices::takeAssetCollectionRequest() {
    return std::exchange(m_assetCollectionRequested, false);
}

void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile, SystemFaults* faults) {
    scheduler.run(phase, context, profile, faults);
    if (context.services == nullptr) {
        return;
    }
    context.services->flush(context.scene);
    if (context.application != nullptr && context.services->takeAssetCollectionRequest()) {
        context.application->control.requestAssetCollection();
    }
}
