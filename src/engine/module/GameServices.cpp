#include "GameServices.hpp"

#include "GameEvents.hpp"

#include <utility>

GameServices::GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger)
    : m_prefabs(assets, std::move(documents))
    , m_debugDraw(debugDraw)
    , m_log(logger, "gameplay") {}

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
    m_commands.flush(scene, m_prefabs, m_events, m_log);
}

void GameServices::publishTriggerEvents(std::span<const TriggerEvent> events) {
    for (const TriggerEvent& event : events) {
        if (event.type == TriggerEventType::Entered) {
            m_events.publish(TriggerEntered{.sensor = event.sensorId, .other = event.otherId});
        } else {
            m_events.publish(TriggerExited{.sensor = event.sensorId, .other = event.otherId});
        }
    }
}

void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile) {
    scheduler.run(phase, context, profile);
    if (context.services != nullptr) {
        context.services->flush(context.scene);
    }
}
