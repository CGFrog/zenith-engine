#pragma once

#include "Effects.hpp"
#include "EntityCommandBuffer.hpp"
#include "EventQueue.hpp"
#include "SessionResources.hpp"
#include "SystemScheduler.hpp"
#include "Timers.hpp"
#include "engine/application/SimulationClock.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/gameplay/Random.hpp"
#include "engine/navigation/NavigationCrowd.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SaveGame.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

class AssetManager;
class BehaviourRegistry;
class DebugDraw;

// When a session's navigation mesh is baked (GameServices::
// setNavigationBakePolicy).
enum class NavigationBakePolicy {
    // GameServices::navigation bakes the scene's static colliders the first
    // time it is asked, which zenith.nav_agents does as soon as an agent
    // exists. Suits hand-authored levels; the default.
    Lazy,
    // Only bakeNavigation bakes. Until it has run, navigation returns an
    // empty mesh and zenith.nav_agents leaves agents waiting with their
    // destinations (logged once). Suits games that generate their levels at
    // runtime, so enemies spawned before the bake do not trigger a bake of
    // a half-built level.
    Explicit,
};

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

// Gameplay mechanisms a host gives its systems for one play session: prefab
// spawning, deferred scene changes, events, timers, effects, and debug
// drawing. Hosts
// create it when gameplay starts (a game's scene loads, the editor enters
// Play or Simulate) and destroy it when gameplay ends. Systems reach it
// through SystemContext::services, which is null in Edit mode.
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

    PrefabSystem& prefabs();
    EntityCommandBuffer& commands();
    EventQueue& events();
    // The session's own data (definition tables, run state), one value per
    // SessionResource type, destroyed with the services: when gameplay
    // stops, and before a module reload.
    SessionResources& resources();

    // resources().get<Resource>(): the session's Resource, default-
    // constructed on first use.
    template <SessionResource Resource>
        requires std::default_initializable<Resource>
    Resource& resource() {
        return m_resources.get<Resource>();
    }

    // assetPath (for example "data/items.json", as prefab and clip
    // references are written) resolved against the session's asset root,
    // for loading files such as a DefinitionTable. A path that is empty,
    // absolute, or leaves the asset root is logged and gives an empty path.
    std::filesystem::path assetPath(std::string_view assetPath) const;
    Timers& timers();
    // Short-lived effects that remove themselves when they end.
    Effects& effects();
    // The session's save slots, or null when the host gives none (tests, the
    // editor without a project). scene is the scene document the session
    // plays, which saves are recorded against.
    void setSaveGames(SaveGameStore saves, std::filesystem::path scene);
    SaveGameStore* saveGames();
    // Saves scene into slot of the session's save games. Loading belongs to
    // the game's flow (it replaces the scene): SaveGameStore::load.
    SaveGameResult saveGame(const Scene& scene, std::string_view slot, std::string title = {});
    // The session's navigation mesh. Under the Lazy bake policy it is baked
    // from scene's static colliders the first time it is asked for (a failed
    // bake is logged once and leaves it empty); under Explicit it stays empty
    // until bakeNavigation. Bake it again after the static geometry changes.
    NavigationWorld& navigation(const Scene& scene);
    // The session's navigation mesh when a bake has run (or failed, leaving
    // it empty), otherwise null; never bakes. For tools that only look, such
    // as the editor's overlays.
    const NavigationWorld* navigationIfBaked() const;
    // The mesh zenith.nav_agents moves agents on: navigation(scene), or null
    // under Explicit until bakeNavigation has run (logged once), so agents
    // keep their destinations instead of failing on an empty mesh.
    NavigationWorld* agentNavigation(const Scene& scene);
    // Lazy unless the game chooses Explicit; set it before the first
    // zenith.nav_agents run (Update) that sees an agent, for example from
    // the system that generates the level.
    void setNavigationBakePolicy(NavigationBakePolicy policy);
    NavigationBakePolicy navigationBakePolicy() const;
    // Bakes the session's navigation mesh from scene's static colliders now,
    // replacing the old one; for a game that builds or replaces a level at
    // runtime, after its spawn commands applied (the next phase, or a timer).
    // A failed bake leaves no mesh and is returned, not logged. Without
    // settings it uses defaultNavigationSettings().
    NavigationResult bakeNavigation(const Scene& scene);
    NavigationResult bakeNavigation(const Scene& scene, const NavMeshBuildSettings& settings);
    // The same from triangles the game assembled itself (gatherNavigationGeometry,
    // appendMeshGeometry), for example a generated floor; settings.layerMask
    // does not apply.
    NavigationResult bakeNavigation(const NavigationGeometry& geometry);
    NavigationResult bakeNavigation(const NavigationGeometry& geometry, const NavMeshBuildSettings& settings);
    // The settings the lazy bake and bakeNavigation without settings use:
    // the project's (ProjectConfig::navigationSettings), which hosts (the
    // game's runtime, the editor's Play) set when gameplay starts, so a game
    // bakes what the editor previewed. The defaults until set.
    void setDefaultNavigationSettings(const NavMeshBuildSettings& settings);
    const NavMeshBuildSettings& defaultNavigationSettings() const;
    // The session's crowd of navigation agents (zenith.nav_agents), started
    // over with crowdSettings() whenever the mesh is baked again.
    NavigationCrowd& navigationCrowd();
    const NavigationCrowd& navigationCrowd() const;
    const NavCrowdSettings& crowdSettings() const;
    // Applies from the next time the crowd starts over (the next bake).
    void setCrowdSettings(const NavCrowdSettings& settings);
    // The game's own pause and time scale, for menus, inventory screens, and
    // slow motion. The host combines them with its own clock (the editor's
    // pause and time scale; simulationClock). While the game is paused,
    // physics, FixedUpdate, timers, animation, scene audio, and first-person
    // look stop; Update and LateUpdate keep running with zero deltaSeconds,
    // and Ui as usual, so the game can draw menus and unpause itself.
    void setPaused(bool paused);
    bool paused() const;
    // Clamped like SimulationClock::timeScale.
    void setTimeScale(float scale);
    float timeScale() const;
    // host with the game's request applied: the time scales multiply, and the
    // game's pause holds the simulation still even through the host's frame
    // step. Hosts hand the result to the engine (ApplicationScene::
    // simulationClock) and run systems and timers on its simulatedSeconds.
    SimulationClock simulationClock(SimulationClock host = {}) const;
    // The session's audio, for sounds gameplay plays (AudioSystem::play,
    // playAt with follow, stop); null when the host has none, as in tests.
    // Voices in the default Scene scope stop when the session's scene
    // unloads and follow the game's clock; Persistent ones (menu sounds)
    // play on while it is paused. The audio system must outlive the services.
    void setAudio(AudioSystem* audio);
    AudioSystem* audio();
    // Loads the clip at assetPath, relative to the session's asset root (as
    // AudioEmitterComponent::clipAsset is), through the audio system's cache.
    // spatial clips are mixed to mono for playAt.
    AudioLoadResult<AudioClipHandle> loadClip(std::string_view assetPath, bool spatial = true);
    // The session's random stream for name ("layout", "loot", ...): drawing
    // from one stream never changes another. Every stream derives from the
    // session seed, which starts at 0; a game sets it for a new run
    // (randomStreams().reseed, with entropySeed() or a seed the player chose)
    // and saves randomStreams().snapshot() to resume one.
    RandomStream& random(std::string_view name);
    RandomStreams& randomStreams();
    // Lines and shapes drawn this frame in every view; cleared after the
    // frame renders.
    DebugDraw& debugDraw();
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
    PrefabSystem m_prefabs;
    EntityCommandBuffer m_commands;
    EventQueue m_events;
    Timers m_timers;
    DebugDraw& m_debugDraw;
    Logger* m_logger;
    LogChannel m_log;
    std::shared_ptr<const BehaviourRegistry> m_behaviours;
    GameplayPhysicsEvents m_physicsEvents;
    Effects m_effects{m_commands, m_timers, m_log};
    AssetManager& m_assets;
    SceneDocumentOptions m_documents;
    std::optional<SaveGameStore> m_saveGames;
    std::filesystem::path m_savedScene;
    NavigationWorld m_navigation;
    NavMeshBuildSettings m_defaultNavigationSettings;
    // A bake ran (or failed): navigation() does not bake lazily any more.
    bool m_navigationBaked{false};
    NavigationBakePolicy m_navigationBakePolicy{NavigationBakePolicy::Lazy};
    bool m_reportedWaitingAgents{false};
    NavigationCrowd m_crowd;
    NavCrowdSettings m_crowdSettings;
    bool m_paused{false};
    float m_timeScale{1.0f};
    RandomStreams m_random;
    AudioSystem* m_audio{nullptr};
    bool m_assetCollectionRequested{false};
    // Last, so it is destroyed first: resources may refer to the rest.
    SessionResources m_resources;
};

// Runs one phase of systems and then, when context.services is set, flushes
// the entity commands they recorded and hands a requested asset collection to
// context.application (it stays pending without one). Every host runs systems through this, so
// commands apply at the same points in the game and in the editor. profile and
// faults are the host's session state SystemScheduler::run records into.
void runGameplayPhase(const SystemScheduler& scheduler, SystemPhase phase, const SystemContext& context,
                      SystemProfile* profile = nullptr, SystemFaults* faults = nullptr);
