// CPU-only coverage of the gameplay services systems receive: deferred entity
// commands and their flush points, double-buffered events, and timers on
// simulated time, the game's pause and time scale, session resources, and
// asset paths.

#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/DefinitionTable.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/GameEvents.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

struct DoorOpened {
    static constexpr std::string_view eventKey = "test.door_opened";
    int door{0};
};

struct ScoreChanged {
    static constexpr std::string_view eventKey = "test.score_changed";
    int score{0};
};

Entity findNamed(Scene& scene, std::string_view name) {
    for (const Entity entity : scene.registry().view<NameComponent>()) {
        if (scene.registry().get<NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

struct ServicesFixture {
    explicit ServicesFixture(std::filesystem::path assetRoot = {})
        : services(assets, {.assetRoot = std::move(assetRoot)}, debugDraw, &logger) {}

    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    Scene scene;
    GameServices services;
};

void testCommandsApplyAtFlush() {
    ServicesFixture fixture;
    Scene& scene = fixture.scene;
    const Entity parent = scene.createEntity("Parent");
    scene.registry().get<TransformComponent>(parent).local = glm::translate(glm::mat4(1.0f), {3.0f, 0.0f, 0.0f});
    const Entity doomed = scene.createEntity("Doomed");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, doomed);
    scene.updateWorldTransforms();

    EntityCommandBuffer& commands = fixture.services.commands();
    Entity spawnedInCallback = entt::null;
    commands.spawnEmpty(
        "Spawned", {.parent = scene.id(parent), .localTransform = glm::translate(glm::mat4(1.0f), {0.0f, 1.0f, 0.0f})},
        [&spawnedInCallback](Scene&, Entity entity) { spawnedInCallback = entity; });
    commands.destroy(scene.id(doomed));
    const SceneEntityId childId = scene.id(child);
    const SceneEntityId doomedId = scene.id(doomed);
    expect(commands.size() == 2 && findNamed(scene, "Spawned") == entt::null &&
               scene.findEntity(doomedId) != entt::null,
           "recording commands leaves the scene unchanged");

    fixture.services.flush(scene);
    const Entity spawned = findNamed(scene, "Spawned");
    expect(spawned != entt::null && spawned == spawnedInCallback,
           "a flush spawns and hands the entity to its callback");
    expect(spawned != entt::null && scene.registry().get<RelationshipComponent>(spawned).parent == parent,
           "a spawn goes under its parent");
    scene.updateWorldTransforms();
    expect(spawned != entt::null &&
               glm::all(glm::equal(glm::vec3(scene.registry().get<TransformComponent>(spawned).world[3]),
                                   glm::vec3(3.0f, 1.0f, 0.0f))),
           "a spawn gets its placement's local transform");
    expect(scene.findEntity(doomedId) == entt::null && scene.findEntity(childId) == entt::null,
           "a flush destroys an entity with its descendants");
    expect(commands.empty(), "a flush forgets the commands it applied");

    // Commands for entities that are gone are skipped; a reparent keeps the
    // world transform.
    commands.destroy(doomedId);
    commands.reparent(scene.id(spawned), {});
    fixture.services.flush(scene);
    scene.updateWorldTransforms();
    expect(scene.registry().get<RelationshipComponent>(spawned).parent == entt::null &&
               glm::all(glm::equal(glm::vec3(scene.registry().get<TransformComponent>(spawned).world[3]),
                                   glm::vec3(3.0f, 1.0f, 0.0f))),
           "a reparent to the root keeps the world transform");

    // A callback's own commands apply in the same flush.
    commands.spawnEmpty("Outer", {}, [&commands](Scene&, Entity) { commands.spawnEmpty("Inner"); });
    fixture.services.flush(scene);
    expect(findNamed(scene, "Outer") != entt::null && findNamed(scene, "Inner") != entt::null,
           "commands recorded by a callback apply in the same flush");

    fixture.services.beginFrame();
    const std::span<const EntitySpawned> spawnedEvents = fixture.services.events().read<EntitySpawned>();
    const std::span<const EntityDestroyed> destroyedEvents = fixture.services.events().read<EntityDestroyed>();
    expect(spawnedEvents.size() == 3 && spawnedEvents.front().entity == scene.id(spawned),
           "each spawn publishes EntitySpawned");
    expect(destroyedEvents.size() == 1 && destroyedEvents.front().entity == doomedId,
           "each destroy publishes EntityDestroyed once");
}

void testCommandsRunInPhases() {
    ServicesFixture fixture;
    auto registry = std::make_shared<SystemRegistry>();
    size_t entitiesSeenBySecond = 0;
    registry->add({.name = "spawner", .order = 0, .run = [](const SystemContext& context) {
                       context.services->commands().spawnEmpty("From system");
                   }});
    registry->add({.name = "counter", .order = 1, .run = [&entitiesSeenBySecond](const SystemContext& context) {
                       entitiesSeenBySecond = context.scene.registry().view<NameComponent>().size();
                   }});
    const SystemScheduler scheduler(registry);
    runGameplayPhase(scheduler, SystemPhase::Update,
                     {.scene = fixture.scene, .mode = SimulationMode::Play, .services = &fixture.services});
    expect(entitiesSeenBySecond == 0, "a later system of the same phase does not see a deferred spawn");
    expect(findNamed(fixture.scene, "From system") != entt::null, "the spawn applies when the phase ends");
}

void testPrefabSpawn() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_game_services_tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    {
        ServicesFixture authoring(root);
        const Entity crate = authoring.scene.createEntity("Crate");
        authoring.scene.setParent(authoring.scene.createEntity("Lid"), crate);
        PrefabCapture capture;
        const PrefabResult captured =
            authoring.services.prefabs().capture(authoring.scene, crate, "crate.zprefab", capture);
        expect(captured.success && savePrefab(root / "crate.zprefab", capture.document).success,
               "a test prefab is written");
    }
    ServicesFixture fixture(root);
    fixture.services.commands().spawn("crate.zprefab");
    fixture.services.commands().spawn("missing.zprefab");
    fixture.services.flush(fixture.scene);
    const Entity crate = findNamed(fixture.scene, "Crate");
    expect(crate != entt::null && fixture.scene.registry().get<RelationshipComponent>(crate).children.size() == 1,
           "a prefab spawn instantiates the whole prefab");
    expect(fixture.services.commands().empty(), "a failing spawn is skipped without stopping the flush");
    std::filesystem::remove_all(root, error);
}

// Writes a prefab whose root has the emitter.
void writeEffectPrefab(const std::filesystem::path& root, const std::string& name,
                       const ParticleEmitterComponent& emitter) {
    ServicesFixture authoring(root);
    const Entity effect = authoring.scene.createEntity(name);
    authoring.scene.registry().emplace<ParticleEmitterComponent>(effect, emitter);
    PrefabCapture capture;
    const PrefabResult captured =
        authoring.services.prefabs().capture(authoring.scene, effect, name + ".zprefab", capture);
    expect(captured.success && savePrefab(root / (name + ".zprefab"), capture.document).success,
           "an effect prefab is written");
}

void testEffects() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_game_services_effects";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    writeEffectPrefab(root, "Spark", {.spawnRate = 0.0f, .burstCount = 10, .lifetimeMin = 0.25f, .lifetimeMax = 0.5f});
    writeEffectPrefab(root, "Smoke", {.spawnRate = 10.0f, .lifetimeMax = 2.0f});

    std::string message;
    Scene scene;
    const Entity spark = scene.createEntity("Spark");
    scene.registry().emplace<ParticleEmitterComponent>(spark, ParticleEmitterComponent{.spawnRate = 0.0f});
    const Entity child = scene.createEntity("Child");
    scene.registry().emplace<ParticleEmitterComponent>(
        child, ParticleEmitterComponent{.spawnRate = 0.0f, .burstCount = 1, .lifetimeMax = 3.0f});
    scene.setParent(child, spark);
    expect(derivedEffectLifetime(scene, spark, message) == 3.0f,
           "a one-shot effect lasts its longest particle lifetime");
    scene.registry().get<ParticleEmitterComponent>(child).burstInterval = 1.0f;
    expect(!derivedEffectLifetime(scene, spark, message) && !message.empty(),
           "a repeating emitter has no derived lifetime");

    ServicesFixture fixture(root);
    Effects& effects = fixture.services.effects();
    effects.spawn("Spark.zprefab", glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f}));
    expect(findNamed(fixture.scene, "Spark") == entt::null, "an effect spawns at the next flush");
    fixture.services.flush(fixture.scene);
    const Entity spawned = findNamed(fixture.scene, "Spark");
    expect(spawned != entt::null && effects.activeCount() == 1 &&
               fixture.scene.registry().get<TransformComponent>(spawned).local[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f),
           "an effect plays at its transform");
    fixture.services.advanceTimers(fixture.scene, 0.0f);
    fixture.services.advanceTimers(fixture.scene, 0.4f);
    expect(findNamed(fixture.scene, "Spark") != entt::null, "an effect lasts its derived lifetime");
    fixture.services.advanceTimers(fixture.scene, 0.2f);
    expect(findNamed(fixture.scene, "Spark") == entt::null && effects.activeCount() == 0,
           "an effect is destroyed when it ends");

    effects.spawn("Smoke.zprefab", glm::mat4(1.0f));
    fixture.services.flush(fixture.scene);
    expect(findNamed(fixture.scene, "Smoke") == entt::null && effects.activeCount() == 0,
           "a continuous effect without a lifetime is refused and destroyed");
    effects.spawn("Smoke.zprefab", glm::mat4(1.0f), {.lifetime = 1.0f});
    fixture.services.flush(fixture.scene);
    fixture.services.advanceTimers(fixture.scene, 0.9f);
    expect(findNamed(fixture.scene, "Smoke") != entt::null, "an explicit lifetime keeps a continuous effect");
    fixture.services.advanceTimers(fixture.scene, 0.2f);
    expect(findNamed(fixture.scene, "Smoke") == entt::null, "an explicit lifetime ends a continuous effect");

    effects.spawn("Spark.zprefab", glm::mat4(1.0f), {.pooled = true});
    fixture.services.flush(fixture.scene);
    const Entity pooled = findNamed(fixture.scene, "Spark");
    const SceneEntityId pooledId = fixture.scene.id(pooled);
    fixture.services.advanceTimers(fixture.scene, 0.6f);
    const auto* parkedEmitter = fixture.scene.registry().try_get<ParticleEmitterComponent>(pooled);
    expect(parkedEmitter != nullptr && !parkedEmitter->emitting && effects.parkedCount("Spark.zprefab") == 1,
           "a pooled effect that ends is parked with its emitters stopped");
    const uint32_t generation = parkedEmitter != nullptr ? parkedEmitter->restartGeneration : 0;
    effects.spawn("Spark.zprefab", glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f}), {.pooled = true});
    fixture.services.flush(fixture.scene);
    const Entity reused = fixture.scene.findEntity(pooledId);
    const auto* restarted =
        reused != entt::null ? fixture.scene.registry().try_get<ParticleEmitterComponent>(reused) : nullptr;
    expect(reused == pooled && restarted != nullptr && restarted->emitting &&
               restarted->restartGeneration == generation + 1 && effects.parkedCount("Spark.zprefab") == 0 &&
               fixture.scene.registry().get<TransformComponent>(reused).local[3].x == 5.0f,
           "a pooled spawn reuses the parked instance, moved and restarted");
    expect(fixture.scene.registry().view<ParticleEmitterComponent>().size() == 1,
           "reusing an instance spawns nothing new");
    fixture.services.advanceTimers(fixture.scene, 0.6f);
    expect(effects.parkedCount("Spark.zprefab") == 1 && effects.activeCount() == 0, "the reused instance parks again");
    std::filesystem::remove_all(root, error);
}

void testEventsLastOneFrame() {
    EventQueue events;
    events.publish(DoorOpened{1});
    expect(events.read<DoorOpened>().empty(), "an event is not readable in the frame it was published");
    events.advanceFrame();
    events.publish(DoorOpened{2});
    events.publish(ScoreChanged{10});
    expect(events.read<DoorOpened>().size() == 1 && events.read<DoorOpened>().front().door == 1,
           "an event is readable in the next frame");
    expect(events.read<ScoreChanged>().empty(), "event types are independent");
    events.advanceFrame();
    expect(events.read<DoorOpened>().size() == 1 && events.read<DoorOpened>().front().door == 2,
           "last frame's events replace the ones before");
    expect(events.read<ScoreChanged>().size() == 1 && events.read<ScoreChanged>().front().score == 10,
           "each type keeps publication order");
    events.advanceFrame();
    expect(events.read<DoorOpened>().empty() && events.read<ScoreChanged>().empty(),
           "events are dropped after one frame");
    events.publish(DoorOpened{3});
    events.clear();
    events.advanceFrame();
    expect(events.read<DoorOpened>().empty(), "clear drops pending events");
}

void testTimers() {
    Scene scene;
    Timers timers;
    const LogChannel log;
    std::vector<std::string> fired;
    // Times are exact in binary, so deadlines compare without rounding.
    timers.start(0.75f, TimerRepeat::Once, [&fired](TimerContext&) { fired.push_back("once"); });
    const TimerHandle repeating =
        timers.start(0.5f, TimerRepeat::Repeat, [&fired](TimerContext&) { fired.push_back("repeat"); });
    expect(!timers.start(0.0f, TimerRepeat::Repeat, [](TimerContext&) {}), "a repeating timer needs an interval");

    timers.advance(scene, 0.25f, log);
    expect(fired.empty(), "nothing fires before its time");
    // Time scale 2: a 0.25 s frame advances 0.5 s of simulated time.
    timers.advance(scene, 0.25f * 2.0f, log);
    expect(fired == std::vector<std::string>{"repeat", "once"}, "due timers fire earliest first");
    // Paused: no simulated time passes.
    timers.advance(scene, 0.0f, log);
    expect(fired.size() == 2, "a paused clock fires nothing");
    timers.advance(scene, 1.0f, log);
    expect(fired.size() == 4 && fired[2] == "repeat" && fired[3] == "repeat",
           "a repeating timer fires once per elapsed interval");
    expect(timers.cancel(repeating) && !timers.active(repeating) && timers.size() == 0,
           "a cancelled timer stops; a finished one-shot is gone");

    const Entity owner = scene.createEntity("Owner");
    bool ownerFired = false;
    timers.start(1.0f, TimerRepeat::Once, [&ownerFired](TimerContext&) { ownerFired = true; }, scene.id(owner));
    scene.destroyEntity(owner);
    timers.advance(scene, 2.0f, log);
    expect(!ownerFired && timers.size() == 0, "a timer whose owner is gone is dropped without firing");

    // A callback may cancel itself.
    TimerHandle self;
    int selfRuns = 0;
    self = timers.start(0.1f, TimerRepeat::Repeat, [&](TimerContext& context) {
        ++selfRuns;
        timers.cancel(context.timer);
    });
    timers.advance(scene, 1.0f, log);
    expect(selfRuns == 1 && !timers.active(self), "a repeating timer can cancel itself from its callback");
}

// Errors the fixture's logger received, oldest first.
std::vector<std::string> loggedErrors(const Logger& logger) {
    std::vector<std::string> errors;
    for (const LogEntry& entry : logger.entries()) {
        if (entry.level == LogLevel::Error) {
            errors.push_back(entry.message);
        }
    }
    return errors;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void testThrowingTimersAreContained() {
    Logger logger{LoggerConfig{.echoToStderr = false}};
    const LogChannel log(&logger, "gameplay");
    Scene scene;
    Timers timers;
    int faultyRuns = 0;
    int healthyRuns = 0;
    const TimerHandle faulty = timers.start(0.5f, TimerRepeat::Repeat, [&faultyRuns](TimerContext&) {
        ++faultyRuns;
        throw std::runtime_error("timer exploded");
    });
    const TimerHandle unknown = timers.start(0.5f, TimerRepeat::Once, [](TimerContext&) { throw 42; });
    timers.start(0.5f, TimerRepeat::Repeat, [&healthyRuns](TimerContext&) { ++healthyRuns; });

    timers.advance(scene, 0.5f, log);
    timers.advance(scene, 1.0f, log);
    expect(faultyRuns == 1 && !timers.active(faulty) && !timers.active(unknown),
           "a throwing timer is dropped, even a repeating one");
    expect(healthyRuns == 3, "the other timers keep running after one throws");
    const std::vector<std::string> errors = loggedErrors(logger);
    expect(errors.size() == 2 && contains(errors[0], "timer exploded") && contains(errors[0], "cancelled") &&
               contains(errors[1], "an unknown exception"),
           "each throwing timer is logged once with what it threw");
}

void testThrowingSpawnCallbacksAreContained() {
    ServicesFixture fixture;
    Scene& scene = fixture.scene;
    EntityCommandBuffer& commands = fixture.services.commands();
    commands.spawnEmpty("Faulty", {}, [](Scene&, Entity) { throw std::runtime_error("spawn callback exploded"); });
    bool laterCallbackRan = false;
    commands.spawnEmpty("Healthy", {}, [&laterCallbackRan](Scene&, Entity) { laterCallbackRan = true; });
    fixture.services.flush(scene);
    expect(findNamed(scene, "Faulty") != entt::null && findNamed(scene, "Healthy") != entt::null && laterCallbackRan,
           "a throwing spawn callback keeps its entity and the commands after it");
    const std::vector<std::string> errors = loggedErrors(fixture.logger);
    expect(errors.size() == 1 && contains(errors[0], "spawn callback exploded"),
           "a throwing spawn callback is logged with what it threw");

    // Through the services: a throwing timer during advanceTimers is logged
    // and the commands other timers recorded still apply.
    fixture.services.timers().start(0.1f, TimerRepeat::Once, [](TimerContext&) { throw std::runtime_error("boom"); });
    fixture.services.timers().start(0.1f, TimerRepeat::Once,
                                    [&commands](TimerContext&) { commands.spawnEmpty("From timer"); });
    fixture.services.advanceTimers(scene, 1.0f);
    expect(findNamed(scene, "From timer") != entt::null && loggedErrors(fixture.logger).size() == 2,
           "advanceTimers contains a throwing timer and flushes the others' commands");
}
} // namespace

void testLogChannels() {
    ServicesFixture fixture;
    expect(fixture.services.log().category() == "gameplay", "the game's own channel is gameplay");
    const LogChannel weapons = fixture.services.log("weapons");
    expect(weapons.category() == "gameplay.weapons" && weapons.logger() == &fixture.logger,
           "a named channel is gameplay.<name> on the same logger");
}

void testGameClock() {
    ServicesFixture fixture;
    GameServices& services = fixture.services;
    expect(!services.paused() && services.timeScale() == 1.0f, "a session starts running at normal speed");
    const SimulationClock idle = services.simulationClock();
    expect(!idle.paused && idle.timeScale == 1.0f, "an idle game leaves the host's clock as it is");

    services.setTimeScale(0.5f);
    expect(services.simulationClock({.timeScale = 2.0f}).timeScale == 1.0f,
           "the game's time scale multiplies the host's");
    services.setTimeScale(100.0f);
    expect(services.timeScale() == SimulationClock::maximumTimeScale, "the game's time scale is clamped");
    expect(services.simulationClock({.timeScale = 4.0f}).timeScale == SimulationClock::maximumTimeScale,
           "the combined time scale is clamped");
    services.setTimeScale(std::numeric_limits<float>::quiet_NaN());
    expect(services.timeScale() == 1.0f, "a NaN time scale becomes 1");

    services.setPaused(true);
    const SimulationClock paused = services.simulationClock();
    expect(paused.paused && simulatedSeconds(paused, 0.016f) == 0.0f && audioPlaybackRate(paused) == 0.0f,
           "a paused game holds simulated time and audio still");
    const SimulationClock stepping = services.simulationClock({.paused = true, .frameStepSeconds = 1.0f / 60.0f});
    expect(stepping.paused && simulatedSeconds(stepping, 0.016f) == 0.0f,
           "the game's pause holds through the editor's frame step");
    services.setPaused(false);
    expect(simulatedSeconds(services.simulationClock({.paused = true, .frameStepSeconds = 0.25f}), 0.016f) == 0.25f,
           "a running game keeps the host's frame step");
}

namespace {
// Records the destruction of the resources below, in order.
std::vector<std::string>* destroyedResources = nullptr;

struct RunState {
    static constexpr std::string_view resourceKey = "test.run_state";

    RunState() = default;

    explicit RunState(uint32_t startFloor)
        : floor(startFloor) {}

    RunState(const RunState&) = delete;
    RunState& operator=(const RunState&) = delete;

    ~RunState() {
        if (destroyedResources != nullptr) {
            destroyedResources->push_back("run state " + std::to_string(floor));
        }
    }

    uint32_t floor{1};
};

struct ScoreBoard {
    static constexpr std::string_view resourceKey = "test.score_board";

    ~ScoreBoard() {
        if (destroyedResources != nullptr) {
            destroyedResources->push_back("score board");
        }
    }

    int best{0};
};

void testSessionResources() {
    std::vector<std::string> destroyed;
    destroyedResources = &destroyed;
    {
        ServicesFixture fixture;
        GameServices& services = fixture.services;
        expect(services.resources().find<RunState>() == nullptr, "a new session has no resources");
        RunState& state = services.resource<RunState>();
        expect(state.floor == 1 && &services.resource<RunState>() == &state,
               "a resource is default-constructed once and found again");
        state.floor = 3;
        services.resources().get<ScoreBoard>().best = 40;
        const RunState& replaced = services.resources().emplace<RunState>(7u);
        expect(replaced.floor == 7 && destroyed == std::vector<std::string>{"run state 3"},
               "emplacing a resource replaces (destroys) the previous value");
        expect(services.resources().erase<ScoreBoard>() && !services.resources().erase<ScoreBoard>() &&
                   services.resources().find<ScoreBoard>() == nullptr,
               "erasing a resource destroys it once");
        services.resources().get<ScoreBoard>();
        destroyed.clear();
    }
    expect((destroyed == std::vector<std::string>{"score board", "run state 7"}),
           "destroying the services destroys their resources, newest first");
    destroyedResources = nullptr;
}

// The session's resources end with the session: stopping gameplay destroys
// them (the editor stops it before a module unloads or reloads), and the next
// session starts empty.
void testResourcesEndWithTheSession() {
    std::vector<std::string> destroyed;
    destroyedResources = &destroyed;
    Logger logger;
    AssetManager assets(&logger);
    Scene scene;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    ApplicationContext application{scene, assets, physics, audio, control, rendering, gameUi, logger};
    GameplayRunner runner(builtInGameplay());
    runner.start(application, {});
    runner.services()->resource<RunState>().floor = 4;
    runner.stop(application);
    expect(destroyed == std::vector<std::string>{"run state 4"}, "stopping gameplay destroys the session's resources");
    runner.start(application, {});
    expect(runner.services()->resources().find<RunState>() == nullptr, "a new session starts without resources");
    runner.stop(application);
    destroyedResources = nullptr;
}

struct ItemDef {
    std::string id;
    float damage{0.0f};
};

// A definition table kept as a session resource.
struct ItemTable {
    static constexpr std::string_view resourceKey = "test.items";

    DefinitionTable<ItemDef> table{Fields<ItemDef>().add("id", &ItemDef::id).add("damage", &ItemDef::damage)};
};

void testAssetPaths() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_game_services_asset_paths";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "data");
    {
        std::ofstream output(root / "data" / "items.json");
        output << R"([{"id": "sword", "damage": 4}])";
    }
    {
        ServicesFixture fixture(root);
        GameServices& services = fixture.services;
        expect(services.assetPath("data/items.json") == root / "data" / "items.json",
               "an asset path resolves against the session's asset root");
        expect(services.assetPath("data/../data/items.json") == root / "data" / "items.json",
               "an asset path is normalized");
        expect(services.assetPath("").empty() && services.assetPath("../outside.json").empty() &&
                   services.assetPath("data/../../outside.json").empty() &&
                   services.assetPath((root / "data" / "items.json").generic_string()).empty(),
               "empty, escaping, and absolute asset paths give no path");
        ItemTable& items = services.resource<ItemTable>();
        const DefinitionTableResult loaded = items.table.load(services.assetPath("data/items.json"));
        expect(loaded.success && items.table.find("sword") != nullptr && items.table.find("sword")->damage == 4.0f,
               "a definition table loads from an asset path into a session resource");
    }
    std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
    testCommandsApplyAtFlush();
    testCommandsRunInPhases();
    testPrefabSpawn();
    testEffects();
    testEventsLastOneFrame();
    testTimers();
    testThrowingTimersAreContained();
    testThrowingSpawnCallbacksAreContained();
    testLogChannels();
    testGameClock();
    testSessionResources();
    testResourcesEndWithTheSession();
    testAssetPaths();
    if (failures == 0) {
        std::cout << "Game services tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
