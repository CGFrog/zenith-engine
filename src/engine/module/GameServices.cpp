#include "GameServices.hpp"

#include "GameEvents.hpp"
#include "engine/application/Application.hpp"
#include "engine/behaviour/BehaviourScheduler.hpp"

#include <utility>

GameServices::GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger,
                           std::shared_ptr<const BehaviourRegistry> behaviours)
    : m_prefabs(assets, documents)
    , m_debugDraw(debugDraw)
    , m_logger(logger)
    , m_log(logger, "gameplay")
    , m_behaviours(std::move(behaviours))
    , m_assets(assets)
    , m_documents(std::move(documents)) {}

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

void GameServices::setSaveGames(SaveGameStore saves, std::filesystem::path scene) {
    m_saveGames.emplace(std::move(saves));
    m_savedScene = std::move(scene);
}

SaveGameStore* GameServices::saveGames() {
    return m_saveGames ? &*m_saveGames : nullptr;
}

void GameServices::setPaused(bool paused) {
    m_paused = paused;
}

bool GameServices::paused() const {
    return m_paused;
}

void GameServices::setTimeScale(float scale) {
    m_timeScale = clampTimeScale(scale);
}

float GameServices::timeScale() const {
    return m_timeScale;
}

SimulationClock GameServices::simulationClock(SimulationClock host) const {
    host.timeScale = clampTimeScale(clampTimeScale(host.timeScale) * m_timeScale);
    if (m_paused) {
        host.paused = true;
        host.frameStepSeconds = 0.0f;
    }
    return host;
}

void GameServices::setAudio(AudioSystem* audio) {
    m_audio = audio;
}

AudioSystem* GameServices::audio() {
    return m_audio;
}

AudioLoadResult<AudioClipHandle> GameServices::loadClip(std::string_view assetPath, bool spatial) {
    if (m_audio == nullptr) {
        return {.error = "This session has no audio"};
    }
    const std::filesystem::path relative(std::u8string(assetPath.begin(), assetPath.end()));
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "'" + std::string(assetPath) + "' is not a path inside the project's assets"};
    }
    return m_audio->loadClip(m_documents.assetRoot / relative, spatial);
}

NavigationResult GameServices::bakeNavigation(const Scene& scene, const NavMeshBuildSettings& settings) {
    // A later navigation() call keeps this mesh, or the lack of one.
    m_navigationBaked = true;
    return m_navigation.bake(scene, m_assets, settings);
}

NavigationCrowd& GameServices::navigationCrowd() {
    return m_crowd;
}

const NavCrowdSettings& GameServices::crowdSettings() const {
    return m_crowdSettings;
}

void GameServices::setCrowdSettings(const NavCrowdSettings& settings) {
    m_crowdSettings = settings;
}

RandomStream& GameServices::random(std::string_view name) {
    return m_random.stream(name);
}

RandomStreams& GameServices::randomStreams() {
    return m_random;
}

NavigationWorld& GameServices::navigation(const Scene& scene) {
    if (!m_navigationBaked) {
        m_navigationBaked = true;
        if (const NavigationResult result = m_navigation.bake(scene, m_assets); !result) {
            m_log.warning("No navigation mesh: " + result.error);
        }
    }
    return m_navigation;
}

SaveGameResult GameServices::saveGame(const Scene& scene, std::string_view slot, std::string title) {
    if (!m_saveGames) {
        return {.error = "This session has no save games"};
    }
    return m_saveGames->save(slot, scene, m_assets, m_documents, m_savedScene, std::move(title));
}

DebugDraw& GameServices::debugDraw() {
    return m_debugDraw;
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
    m_timers.advance(scene, simulatedSeconds, m_log);
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
