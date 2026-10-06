#pragma once

// GameServices: the per-session hub of gameplay mechanisms, grouped as the world, time, content, and the outside,
// plus events and logging.

#include "EventQueue.hpp"
#include "GameServiceGroups.hpp"
#include "SystemScheduler.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Prefab.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

class AssetManager;
class BehaviourRegistry;
class DebugDraw;

// A physics step's sensor entries and exits and the contacts that began
// between two solid bodies, by scene id, kept for the behaviours' trigger and
// collision hooks. A collision's normal points from first toward second.
// Both lists keep the physics world's deterministic order
// (triggerEventPrecedes and contactEventPrecedes), step after step.
struct GameplayPhysicsEvents {
    struct Trigger {
        bool entered{true};
        SceneEntityId sensor;
        SceneEntityId other;
    };

    struct Collision {
        SceneEntityId first;
        SceneEntityId second;
        glm::vec3 normal{0.0f};
    };

    std::vector<Trigger> triggers;
    std::vector<Collision> collisions;
};

// Gameplay mechanisms a host gives its systems for one play session, in
// four groups: world() (prefab spawning, deferred scene changes, effects),
// time() (timers, the game's pause and time scale), content() (session
// resources, random streams, asset paths), and outside() (audio, navigation,
// saves, debug drawing); events and logging, which every group's users share,
// are its own. Hosts create it when gameplay starts (a game's scene loads, the
// editor enters Play or Simulate) and destroy it when gameplay ends. Systems
// reach it through SystemContext::services, which is null in Edit mode.
//
// Lifetime: commands, timers, event channels, and session resources hold
// game module code, so the services must be destroyed before the module that
// ran in them unloads.
class GameServices {
  public:
    // documents names the gameplay's component types and asset root, for
    // spawning prefabs. assets, debugDraw, and logger must outlive the
    // services; a null logger writes to stderr. behaviours, when set, get
    // their onDestroy hooks from destroy commands.
    GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger,
                 std::shared_ptr<const BehaviourRegistry> behaviours = {});

    GameServices(const GameServices&) = delete;
    GameServices& operator=(const GameServices&) = delete;

    WorldServices& world();
    TimeServices& time();
    const TimeServices& time() const;
    ContentServices& content();
    OutsideServices& outside();
    const OutsideServices& outside() const;

    EventQueue& events();
    const LogChannel& log() const;
    // A channel for one part of the game, category "gameplay.<name>", so the
    // editor's Info panel can show or hide it on its own.
    LogChannel log(std::string_view name) const;

    // Starts a rendered frame: last frame's events become readable.
    void beginFrame();
    // Runs the timers due after simulatedSeconds, then flushes the commands
    // they recorded. The host calls it once per frame after the Update phase.
    void advanceTimers(Scene& scene, float simulatedSeconds);
    // Applies the requested effects, then the recorded entity commands.
    void flush(Scene& scene);
    // Publishes a physics step's sensor events as TriggerEntered and
    // TriggerExited, and keeps them and the step's new contacts between solid
    // bodies for the behaviours, in the order given (PhysicsWorld's order is
    // deterministic). The host calls it after each step.
    void publishPhysicsEvents(const Scene& scene, std::span<const TriggerEvent> triggers,
                              std::span<const ContactEvent> contacts);
    // The physics events kept since the last call.
    GameplayPhysicsEvents takePhysicsEvents();

    // Asks the host to collect the assets nothing leases any more, after the
    // frames in flight retire (DeferredAssetCollection); for example after
    // destroying a roguelike floor's rooms. runGameplayPhase hands the
    // request to the application (ApplicationControl::
    // requestAssetCollection) after the phase's commands applied, so the
    // destroyed entities no longer lease their assets.
    void requestAssetCollection();
    // Whether a collection was requested since the last call.
    bool takeAssetCollectionRequest();

  private:
    Logger* m_logger;
    LogChannel m_log;
    std::shared_ptr<const BehaviourRegistry> m_behaviours;
    EventQueue m_events;
    GameplayPhysicsEvents m_physicsEvents;
    bool m_assetCollectionRequested{false};
    // The groups, in an order that keeps what each refers to alive: the
    // world's effects use the timers and the log; the content, holding the
    // session resources that may refer to the rest, goes first.
    TimeServices m_time;
    WorldServices m_world;
    OutsideServices m_outside;
    ContentServices m_content;
};

// Runs one phase of systems and then, when context.services is set, flushes
// the entity commands they recorded and hands a requested asset collection to
// context.application (it stays pending without one). Every host runs systems through this, so
// commands apply at the same points in the game and in the editor. profile and
// faults are the host's session state SystemScheduler::run records into.
void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile = nullptr, SystemFaults* faults = nullptr);
