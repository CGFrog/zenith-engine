#pragma once

#include "EntityCommandBuffer.hpp"
#include "EventQueue.hpp"
#include "SystemScheduler.hpp"
#include "Timers.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/scene/Prefab.hpp"

class AssetManager;
class DebugDraw;

// Gameplay mechanisms a host gives its systems for one play session: prefab
// spawning, deferred scene changes, events, timers, and debug drawing. Hosts
// create it when gameplay starts (a game's scene loads, the editor enters
// Play or Simulate) and destroy it when gameplay ends. Systems reach it
// through SystemContext::services, which is null in Edit mode.
//
// Lifetime: commands, timers, and event channels hold game module code, so
// the services must be destroyed before the module that ran in them unloads.
class GameServices {
  public:
    // documents names the gameplay's component types and asset root, for
    // spawning prefabs. assets, debugDraw, and logger must outlive the
    // services; a null logger writes to stderr.
    GameServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, Logger* logger);

    GameServices(const GameServices&) = delete;
    GameServices& operator=(const GameServices&) = delete;

    PrefabSystem& prefabs();
    EntityCommandBuffer& commands();
    EventQueue& events();
    Timers& timers();
    // Lines and shapes drawn this frame in every view; cleared after the
    // frame renders.
    DebugDraw& debugDraw();
    const LogChannel& log() const;

    // Starts a rendered frame: last frame's events become readable.
    void beginFrame();
    // Runs the timers due after simulatedSeconds, then flushes the commands
    // they recorded. The host calls it once per frame after the Update phase.
    void advanceTimers(Scene& scene, float simulatedSeconds);
    // Applies the recorded entity commands.
    void flush(Scene& scene);

  private:
    PrefabSystem m_prefabs;
    EntityCommandBuffer m_commands;
    EventQueue m_events;
    Timers m_timers;
    DebugDraw& m_debugDraw;
    LogChannel m_log;
};

// Runs one phase of systems and then, when context.services is set, flushes
// the entity commands they recorded. Every host runs systems through this, so
// commands apply at the same points in the game and in the editor.
void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile = nullptr);
