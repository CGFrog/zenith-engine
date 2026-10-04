// CPU-only coverage of the gameplay services systems receive: deferred entity
// commands and their flush points, double-buffered events, and timers on
// simulated time.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/GameEvents.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
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
    std::vector<std::string> fired;
    // Times are exact in binary, so deadlines compare without rounding.
    timers.start(0.75f, TimerRepeat::Once, [&fired](TimerContext&) { fired.push_back("once"); });
    const TimerHandle repeating =
        timers.start(0.5f, TimerRepeat::Repeat, [&fired](TimerContext&) { fired.push_back("repeat"); });
    expect(!timers.start(0.0f, TimerRepeat::Repeat, [](TimerContext&) {}), "a repeating timer needs an interval");

    timers.advance(scene, 0.25f);
    expect(fired.empty(), "nothing fires before its time");
    // Time scale 2: a 0.25 s frame advances 0.5 s of simulated time.
    timers.advance(scene, 0.25f * 2.0f);
    expect(fired == std::vector<std::string>{"repeat", "once"}, "due timers fire earliest first");
    // Paused: no simulated time passes.
    timers.advance(scene, 0.0f);
    expect(fired.size() == 2, "a paused clock fires nothing");
    timers.advance(scene, 1.0f);
    expect(fired.size() == 4 && fired[2] == "repeat" && fired[3] == "repeat",
           "a repeating timer fires once per elapsed interval");
    expect(timers.cancel(repeating) && !timers.active(repeating) && timers.size() == 0,
           "a cancelled timer stops; a finished one-shot is gone");

    const Entity owner = scene.createEntity("Owner");
    bool ownerFired = false;
    timers.start(1.0f, TimerRepeat::Once, [&ownerFired](TimerContext&) { ownerFired = true; }, scene.id(owner));
    scene.destroyEntity(owner);
    timers.advance(scene, 2.0f);
    expect(!ownerFired && timers.size() == 0, "a timer whose owner is gone is dropped without firing");

    // A callback may cancel itself.
    TimerHandle self;
    int selfRuns = 0;
    self = timers.start(0.1f, TimerRepeat::Repeat, [&](TimerContext& context) {
        ++selfRuns;
        timers.cancel(context.timer);
    });
    timers.advance(scene, 1.0f);
    expect(selfRuns == 1 && !timers.active(self), "a repeating timer can cancel itself from its callback");
}
} // namespace

int main() {
    testCommandsApplyAtFlush();
    testCommandsRunInPhases();
    testPrefabSpawn();
    testEventsLastOneFrame();
    testTimers();
    if (failures == 0) {
        std::cout << "Game services tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
