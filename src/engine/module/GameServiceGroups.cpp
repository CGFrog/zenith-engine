#include "GameServiceGroups.hpp"

#include <utility>

Timers& TimeServices::timers() {
    return m_timers;
}

void TimeServices::setPaused(bool paused) {
    m_paused = paused;
}

bool TimeServices::paused() const {
    return m_paused;
}

void TimeServices::setTimeScale(float scale) {
    m_timeScale = clampTimeScale(scale);
}

float TimeServices::timeScale() const {
    return m_timeScale;
}

SimulationClock TimeServices::simulationClock(SimulationClock host) const {
    host.timeScale = clampTimeScale(clampTimeScale(host.timeScale) * m_timeScale);
    if (m_paused) {
        host.paused = true;
        host.frameStepSeconds = 0.0f;
    }
    return host;
}

WorldServices::WorldServices(AssetManager& assets, SceneDocumentOptions documents, Timers& timers,
                             const LogChannel& log)
    : m_prefabs(assets, std::move(documents))
    , m_effects(m_commands, timers, log) {}

PrefabSystem& WorldServices::prefabs() {
    return m_prefabs;
}

EntityCommandBuffer& WorldServices::commands() {
    return m_commands;
}

Effects& WorldServices::effects() {
    return m_effects;
}

ContentServices::ContentServices(std::filesystem::path assetRoot, const LogChannel& log)
    : m_assetRoot(std::move(assetRoot))
    , m_log(log) {}

SessionResources& ContentServices::resources() {
    return m_resources;
}

std::filesystem::path ContentServices::assetPath(std::string_view assetPath) const {
    const std::filesystem::path relative =
        std::filesystem::path(std::u8string(assetPath.begin(), assetPath.end())).lexically_normal();
    if (relative.empty() || relative.has_root_path() || *relative.begin() == "..") {
        m_log.warning("'" + std::string(assetPath) + "' is not a path inside the project's assets");
        return {};
    }
    return m_assetRoot / relative;
}

RandomStream& ContentServices::random(std::string_view name) {
    return m_random.stream(name);
}

RandomStreams& ContentServices::randomStreams() {
    return m_random;
}

OutsideServices::OutsideServices(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw,
                                 const LogChannel& log)
    : m_assets(assets)
    , m_documents(std::move(documents))
    , m_debugDraw(debugDraw)
    , m_log(log) {}

void OutsideServices::setAudio(AudioSystem* audio) {
    m_audio = audio;
}

AudioSystem* OutsideServices::audio() {
    return m_audio;
}

AudioLoadResult<AudioClipHandle> OutsideServices::loadClip(std::string_view assetPath, bool spatial) {
    if (m_audio == nullptr) {
        return {.error = "This session has no audio"};
    }
    const std::filesystem::path relative(std::u8string(assetPath.begin(), assetPath.end()));
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "'" + std::string(assetPath) + "' is not a path inside the project's assets"};
    }
    return m_audio->loadClip(m_documents.assetRoot / relative, spatial);
}

NavigationWorld& OutsideServices::navigation(const Scene& scene) {
    if (!m_navigationBaked && m_navigationBakePolicy == NavigationBakePolicy::Lazy) {
        m_navigationBaked = true;
        if (const NavigationResult result = m_navigation.bake(scene, m_assets, m_defaultNavigationSettings); !result) {
            m_log.warning("No navigation mesh: " + result.error);
        }
    }
    return m_navigation;
}

const NavigationWorld* OutsideServices::navigationIfBaked() const {
    return m_navigationBaked ? &m_navigation : nullptr;
}

NavigationWorld* OutsideServices::agentNavigation(const Scene& scene) {
    if (m_navigationBakePolicy == NavigationBakePolicy::Explicit && !m_navigationBaked) {
        if (!m_reportedWaitingAgents) {
            m_reportedWaitingAgents = true;
            m_log.warning("Navigation agents wait for OutsideServices::bakeNavigation (the bake policy is Explicit)");
        }
        return nullptr;
    }
    return &navigation(scene);
}

void OutsideServices::setNavigationBakePolicy(NavigationBakePolicy policy) {
    m_navigationBakePolicy = policy;
}

NavigationBakePolicy OutsideServices::navigationBakePolicy() const {
    return m_navigationBakePolicy;
}

NavigationResult OutsideServices::bakeNavigation(const Scene& scene) {
    return bakeNavigation(scene, m_defaultNavigationSettings);
}

NavigationResult OutsideServices::bakeNavigation(const Scene& scene, const NavMeshBuildSettings& settings) {
    // A later navigation() call keeps this mesh, or the lack of one.
    m_navigationBaked = true;
    return m_navigation.bake(scene, m_assets, settings);
}

NavigationResult OutsideServices::bakeNavigation(const NavigationGeometry& geometry) {
    return bakeNavigation(geometry, m_defaultNavigationSettings);
}

NavigationResult OutsideServices::bakeNavigation(const NavigationGeometry& geometry,
                                                 const NavMeshBuildSettings& settings) {
    m_navigationBaked = true;
    return m_navigation.bake(geometry, settings);
}

void OutsideServices::setDefaultNavigationSettings(const NavMeshBuildSettings& settings) {
    m_defaultNavigationSettings = settings;
}

const NavMeshBuildSettings& OutsideServices::defaultNavigationSettings() const {
    return m_defaultNavigationSettings;
}

NavigationCrowd& OutsideServices::navigationCrowd() {
    return m_crowd;
}

const NavigationCrowd& OutsideServices::navigationCrowd() const {
    return m_crowd;
}

const NavCrowdSettings& OutsideServices::crowdSettings() const {
    return m_crowdSettings;
}

void OutsideServices::setCrowdSettings(const NavCrowdSettings& settings) {
    m_crowdSettings = settings;
}

void OutsideServices::setSaveGames(SaveGameStore saves, std::filesystem::path scene) {
    m_saveGames.emplace(std::move(saves));
    m_savedScene = std::move(scene);
}

SaveGameStore* OutsideServices::saveGames() {
    return m_saveGames ? &*m_saveGames : nullptr;
}

SaveGameResult OutsideServices::saveGame(const Scene& scene, std::string_view slot, std::string title) {
    if (!m_saveGames) {
        return {.error = "This session has no save games"};
    }
    return m_saveGames->save(slot, scene, m_assets, m_documents, m_savedScene, std::move(title));
}

DebugDraw& OutsideServices::debugDraw() {
    return m_debugDraw;
}

void OutsideServices::setPointerReleased(bool released) {
    m_pointerReleased = released;
}

bool OutsideServices::pointerReleased() const {
    return m_pointerReleased;
}
