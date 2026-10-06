#pragma once

// The groups of GameServices: the world (prefabs, commands, effects), time (timers, pause, time scale), content
// (session resources, random streams, asset paths), and the outside (audio, navigation, saves, debug drawing).

#include "Effects.hpp"
#include "EntityCommandBuffer.hpp"
#include "SessionResources.hpp"
#include "Timers.hpp"
#include "engine/application/SimulationClock.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/gameplay/Random.hpp"
#include "engine/navigation/NavigationCrowd.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SaveGame.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class AssetManager;
class DebugDraw;

// When a session's navigation mesh is baked (OutsideServices::
// setNavigationBakePolicy).
enum class NavigationBakePolicy {
    // OutsideServices::navigation bakes the scene's static colliders the
    // first time it is asked, which zenith.nav_agents does as soon as an
    // agent exists. Suits hand-authored levels; the default.
    Lazy,
    // Only bakeNavigation bakes. Until it has run, navigation returns an
    // empty mesh and zenith.nav_agents leaves agents waiting with their
    // destinations (logged once). Suits games that generate their levels at
    // runtime, so enemies spawned before the bake do not trigger a bake of
    // a half-built level.
    Explicit,
};

// The session's clock: timers, and the game's own pause and time scale.
class TimeServices {
  public:
    TimeServices() = default;
    TimeServices(const TimeServices&) = delete;
    TimeServices& operator=(const TimeServices&) = delete;

    Timers& timers();
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

  private:
    Timers m_timers;
    bool m_paused{false};
    float m_timeScale{1.0f};
};

// Changes to the scene: spawning prefabs, deferred entity commands, and
// short-lived effects.
class WorldServices {
  public:
    // timers and log must outlive the world, whose effects use them.
    WorldServices(AssetManager& assets, SceneDocumentOptions documents, Timers& timers, const LogChannel& log);
    WorldServices(const WorldServices&) = delete;
    WorldServices& operator=(const WorldServices&) = delete;

    PrefabSystem& prefabs();
    EntityCommandBuffer& commands();
    // Short-lived effects that remove themselves when they end.
    Effects& effects();

  private:
    PrefabSystem m_prefabs;
    EntityCommandBuffer m_commands;
    Effects m_effects;
};

// The session's own data: values per SessionResource type, named random
// streams, and paths to the project's asset files.
class ContentServices {
  public:
    // log must outlive the content.
    ContentServices(std::filesystem::path assetRoot, const LogChannel& log);
    ContentServices(const ContentServices&) = delete;
    ContentServices& operator=(const ContentServices&) = delete;

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
    // The session's random stream for name ("layout", "loot", ...): drawing
    // from one stream never changes another. Every stream derives from the
    // session seed, which starts at 0; a game sets it for a new run
    // (randomStreams().reseed, with entropySeed() or a seed the player chose)
    // and saves randomStreams().snapshot() to resume one.
    RandomStream& random(std::string_view name);
    RandomStreams& randomStreams();

  private:
    std::filesystem::path m_assetRoot;
    const LogChannel& m_log;
    RandomStreams m_random;
    // Last, so it is destroyed first: resources may refer to the rest.
    SessionResources m_resources;
};

// What the session reaches outside the scene: audio, the navigation mesh and
// crowd, save slots, and debug drawing.
class OutsideServices {
  public:
    // assets, debugDraw, and log must outlive the services.
    OutsideServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw, const LogChannel& log);
    OutsideServices(const OutsideServices&) = delete;
    OutsideServices& operator=(const OutsideServices&) = delete;

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

    // The session's save slots, or null when the host gives none (tests, the
    // editor without a project). scene is the scene document the session
    // plays, which saves are recorded against.
    void setSaveGames(SaveGameStore saves, std::filesystem::path scene);
    SaveGameStore* saveGames();
    // Saves scene into slot of the session's save games. Loading belongs to
    // the game's flow (it replaces the scene): SaveGameStore::load.
    SaveGameResult saveGame(const Scene& scene, std::string_view slot, std::string title = {});

    // Lines and shapes drawn this frame in every view; cleared after the
    // frame renders.
    DebugDraw& debugDraw();

  private:
    AssetManager& m_assets;
    SceneDocumentOptions m_documents;
    DebugDraw& m_debugDraw;
    const LogChannel& m_log;
    AudioSystem* m_audio{nullptr};
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
};
