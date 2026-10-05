#include "engine/diagnostics/Logger.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A static floor and a dynamic box three metres above it; returns the box.
Entity addFallingBox(Scene& scene) {
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(5.0f, 0.5f, 5.0f));
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(1.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);
    const Entity box = scene.createEntity("Box");
    scene.registry().get<TransformComponent>(box).local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
    scene.registry().emplace<ColliderComponent>(box, BoxCollider{glm::vec3(0.5f)});
    scene.registry().emplace<RigidBodyComponent>(
        box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .layer = PhysicsLayer::Dynamic});
    return box;
}

// One world of several: its own scene, stepped and drawn independently.
struct WorldUnderTest {
    Scene scene;
    Entity box{entt::null};
    std::unique_ptr<PhysicsWorld> physics = std::make_unique<PhysicsWorld>();

    WorldUnderTest() {
        box = addFallingBox(scene);
        expect(static_cast<bool>(physics->init()), "each of several physics worlds initializes");
    }

    bool stepAndDraw(const AssetManager& assets, int steps) {
        for (int step = 0; step < steps; ++step) {
            if (!physics->step(scene, assets, physics->config().fixedTimeStep)) {
                return false;
            }
        }
        DebugDraw debug;
        return physics->drawDebug(debug) && debug.lineCount() > 0;
    }

    float boxHeight() const {
        return scene.registry().get<TransformComponent>(box).local[3].y;
    }
};

// Worlds share Jolt's process-wide runtime (factory, registered types, debug
// renderer): any number may run at once, and shutting one down in any order
// leaves the others stepping and drawing.
void testWorldsShareJoltRuntime() {
    AssetManager assets;
    for (const bool firstShutsDownFirst : {true, false}) {
        WorldUnderTest first;
        WorldUnderTest second;
        expect(first.stepAndDraw(assets, 10) && second.stepAndDraw(assets, 10),
               "two worlds step and draw debug geometry at the same time");
        WorldUnderTest& closed = firstShutsDownFirst ? first : second;
        WorldUnderTest& survivor = firstShutsDownFirst ? second : first;
        const float before = survivor.boxHeight();
        closed.physics->shutdown();
        closed.physics.reset();
        expect(survivor.stepAndDraw(assets, 30) && survivor.boxHeight() < before,
               "a world keeps simulating and drawing after another world shuts down");
        WorldUnderTest third;
        expect(third.stepAndDraw(assets, 5) && survivor.stepAndDraw(assets, 5),
               "a world created after another shut down shares the live runtime");
    }
    // Every world above is gone, so the runtime was destroyed; a new world
    // creates it again.
    WorldUnderTest fresh;
    expect(fresh.stepAndDraw(assets, 5), "a world initializes again after every world shut down");
    fresh.physics->shutdown();
    expect(static_cast<bool>(fresh.physics->init()) && fresh.stepAndDraw(assets, 5),
           "a world shut down and initialized again steps and draws");
}

LoggerConfig quietLogger() {
    return {.minimumLevel = LogLevel::Debug, .echoToStderr = false};
}

// Warnings the physics world logged whose message contains text.
size_t physicsWarnings(const Logger& logger, const std::string& text) {
    const std::vector<LogEntry> entries = logger.entries();
    return static_cast<size_t>(std::ranges::count_if(entries, [&text](const LogEntry& entry) {
        return entry.category == "physics" && entry.level == LogLevel::Warning &&
               entry.message.find(text) != std::string::npos;
    }));
}

bool stepFor(PhysicsWorld& physics, Scene& scene, const AssetManager& assets, int steps) {
    for (int step = 0; step < steps; ++step) {
        if (!physics.step(scene, assets, physics.config().fixedTimeStep)) {
            return false;
        }
    }
    return true;
}

Entity addBox(Scene& scene, const glm::vec3& position, const char* name = "Box") {
    const Entity box = scene.createEntity(name);
    scene.registry().get<TransformComponent>(box).local = glm::translate(glm::mat4(1.0f), position);
    scene.registry().emplace<ColliderComponent>(box, BoxCollider{glm::vec3(0.5f)});
    scene.registry().emplace<RigidBodyComponent>(
        box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .layer = PhysicsLayer::Dynamic});
    return box;
}

// A body that cannot be built is left out with one warning naming it, and the
// rest of the world keeps simulating, even though every audit retries it.
void testUnbuildableBodiesAreSkipped() {
    Logger logger(quietLogger());
    PhysicsWorld physics(&logger);
    expect(static_cast<bool>(physics.init({.debugAuditInterval = 1})), "physics initializes with frequent audits");
    Scene scene;
    AssetManager assets;
    const Entity box = addFallingBox(scene);
    const Entity dynamicMesh = scene.createEntity("Dynamic Mesh");
    scene.registry().emplace<ColliderComponent>(dynamicMesh, TriangleMeshCollider{});
    scene.registry().emplace<RigidBodyComponent>(
        dynamicMesh, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .layer = PhysicsLayer::Dynamic});
    const Entity flattened = addBox(scene, glm::vec3(2.0f, 3.0f, 0.0f), "Flattened");
    scene.registry().get<TransformComponent>(flattened).local = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
    const Entity character = scene.createEntity("Pinhead");
    scene.registry().emplace<CharacterControllerComponent>(character, CharacterControllerComponent{.radius = 0.0f});

    const float startHeight = scene.registry().get<TransformComponent>(box).local[3].y;
    expect(stepFor(physics, scene, assets, 30), "steps succeed while some bodies cannot be built");
    expect(scene.registry().get<TransformComponent>(box).local[3].y < startHeight,
           "the other bodies keep simulating next to a body that cannot be built");
    expect(physics.stats().skippedBodies == 3, "skipped bodies and characters are counted in the stats");
    expect(physicsWarnings(logger, "'Dynamic Mesh'") == 1 && physicsWarnings(logger, "'Flattened'") == 1 &&
               physicsWarnings(logger, "'Pinhead'") == 1,
           "each skipped body is reported once, by name, despite retries");
    expect(!physics.refreshBody(dynamicMesh, scene, assets),
           "an explicit refresh still reports a body that cannot be built");

    scene.registry().get<TransformComponent>(flattened).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 0.0f));
    scene.markTransformDirty(flattened);
    scene.markPhysicsDirty(flattened);
    scene.registry().get<CharacterControllerComponent>(character).radius = 0.4f;
    expect(stepFor(physics, scene, assets, 1) && physics.stats().skippedBodies == 1,
           "fixed bodies and characters are built on a later step");
    PhysicsRayHit hit;
    expect(physics.raycast({2.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, hit) && hit.entity == flattened,
           "a body built after being skipped joins the simulation");
}

// Bodies beyond maxBodies are skipped with one warning per episode and built
// once removals make room.
void testBodyCapacityIsClamped() {
    Logger logger(quietLogger());
    PhysicsWorld physics(&logger);
    expect(static_cast<bool>(physics.init({.maxBodies = 4})), "physics initializes with four bodies");
    Scene scene;
    AssetManager assets;
    addFallingBox(scene);
    std::vector<Entity> boxes;
    for (int index = 0; index < 4; ++index) {
        boxes.push_back(addBox(scene, glm::vec3(static_cast<float>(index) * 2.0f, 3.0f, 2.0f)));
    }
    expect(stepFor(physics, scene, assets, 10), "steps succeed beyond the body capacity");
    expect(physics.stats().skippedBodies == 2 && physics.stats().bodies == 4,
           "bodies beyond the capacity are skipped and counted");
    expect(physicsWarnings(logger, "body capacity") == 1, "the body capacity is reported once per episode");

    scene.destroyEntity(boxes[0]);
    scene.destroyEntity(boxes[1]);
    expect(stepFor(physics, scene, assets, 1) && physics.stats().skippedBodies == 0 && physics.stats().bodies == 4,
           "skipped bodies are built once removals make room");
    scene.markPhysicsDirty(addBox(scene, glm::vec3(0.0f, 6.0f, 6.0f)));
    expect(stepFor(physics, scene, assets, 5) && physics.stats().skippedBodies == 1,
           "a new body over the capacity is skipped again");
    expect(physicsWarnings(logger, "body capacity") == 2, "a new overflow episode is reported again");
}

// Running out of contact capacity leaves contacts unresolved but keeps the
// step going, with one warning per episode.
void testContactCapacityIsAWarning() {
    Logger logger(quietLogger());
    PhysicsWorld physics(&logger);
    expect(static_cast<bool>(physics.init({.maxContactConstraints = 4})),
           "physics initializes with tiny contact caches");
    Scene scene;
    AssetManager assets;
    addFallingBox(scene);
    for (int index = 0; index < 16; ++index) {
        const float x = static_cast<float>(index % 4) * 1.1f - 1.5f;
        const float z = static_cast<float>(index / 4) * 1.1f - 1.5f;
        addBox(scene, glm::vec3(x, 0.5f, z));
    }
    bool exceeded = false;
    for (int step = 0; step < 30; ++step) {
        expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
               "a step that runs out of contact capacity still succeeds");
        exceeded |= physics.stats().contactCapacityExceeded;
    }
    expect(exceeded, "an exhausted contact capacity is reported in the stats");
    expect(physicsWarnings(logger, "ran out of") == 1, "an exhausted contact capacity is logged once per episode");
}

void testConfigurationLimitsAreValidated() {
    PhysicsWorld physics;
    expect(!physics.init({.maxContactConstraints = 65536, .temporaryAllocatorBytes = 1024 * 1024}),
           "a temporary allocator too small for the contact constraints is rejected");
    expect(!physics.init({.maxBodies = 0}), "a zero body capacity is rejected");
}

// Contact and trigger events sort by scene ids and type, whatever order they
// arrive in.
void testEventOrderIsByScene() {
    const auto contact = [](ContactEventType type, uint64_t first, uint64_t second, float normalX = 0.0f) {
        return ContactEvent{.type = type,
                            .normal = {normalX, 0.0f, 0.0f},
                            .firstId = SceneEntityId{first},
                            .secondId = SceneEntityId{second}};
    };
    const std::vector<ContactEvent> expected{
        contact(ContactEventType::Begin, 1, 4),       contact(ContactEventType::Persist, 4, 1),
        contact(ContactEventType::End, 1, 4),         contact(ContactEventType::Begin, 2, 3, -1.0f),
        contact(ContactEventType::Begin, 2, 3, 1.0f), contact(ContactEventType::Begin, 3, 2),
        contact(ContactEventType::Persist, 7, 2)};
    std::vector<ContactEvent> shuffled{expected[5], expected[2], expected[6], expected[0],
                                       expected[4], expected[3], expected[1]};
    std::ranges::sort(shuffled, contactEventPrecedes);
    bool contactsMatch = shuffled.size() == expected.size();
    for (size_t index = 0; contactsMatch && index < expected.size(); ++index) {
        contactsMatch =
            shuffled[index].type == expected[index].type && shuffled[index].firstId == expected[index].firstId &&
            shuffled[index].secondId == expected[index].secondId && shuffled[index].normal == expected[index].normal;
    }
    expect(contactsMatch, "contacts order by the pair's lower then higher scene id, type, first id, and normal");

    const auto trigger = [](TriggerEventType type, uint64_t sensor, uint64_t other) {
        return TriggerEvent{.type = type, .sensorId = SceneEntityId{sensor}, .otherId = SceneEntityId{other}};
    };
    std::vector<TriggerEvent> triggers{
        trigger(TriggerEventType::Exited, 5, 1), trigger(TriggerEventType::Entered, 2, 9),
        trigger(TriggerEventType::Entered, 5, 1), trigger(TriggerEventType::Entered, 2, 3)};
    std::ranges::sort(triggers, triggerEventPrecedes);
    expect(triggers[0].otherId.value == 3 && triggers[1].otherId.value == 9 &&
               triggers[2].type == TriggerEventType::Entered && triggers[3].type == TriggerEventType::Exited,
           "triggers order by sensor id, other id, then Entered before Exited");
}

// A contact carries a point where the bodies touch: a box landing on a floor
// touches it at the floor's top.
void testContactPoints() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "the contact point world initializes");
    Scene scene;
    AssetManager assets;
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local = glm::translate(glm::mat4(1.0f), {0.0f, -0.5f, 0.0f});
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(5.0f, 0.5f, 5.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);
    addBox(scene, {1.0f, 1.0f, 2.0f});
    std::optional<ContactEvent> begin;
    for (int step = 0; step < 120 && !begin; ++step) {
        expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
               "the contact point world steps");
        for (const ContactEvent& contact : physics.contactEvents()) {
            if (contact.type == ContactEventType::Begin) {
                begin = contact;
            }
        }
    }
    expect(begin.has_value() && std::abs(begin->point.y) < 0.1f && std::abs(begin->point.x - 1.0f) <= 0.55f &&
               std::abs(begin->point.z - 2.0f) <= 0.55f,
           "a contact's point is where the box meets the floor");
    physics.shutdown();
}

struct TriggerRecord {
    TriggerEventType type;
    uint64_t sensor;
    uint64_t other;

    bool operator==(const TriggerRecord&) const = default;
};

// Several sensors, each with a box falling through it, built with their
// entities created in the given order of zones; scene ids are fixed per zone,
// so only creation (and so Jolt body) order differs. Returns the trigger
// events of every step until the boxes leave.
std::vector<TriggerRecord> simultaneousTriggerEvents(std::span<const uint32_t> zoneOrder) {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "the trigger order world initializes");
    Scene scene;
    AssetManager assets;
    for (const uint32_t zone : zoneOrder) {
        const glm::vec3 center(static_cast<float>(zone) * 4.0f, 0.0f, 0.0f);
        Entity sensor = entt::null;
        Entity box = entt::null;
        // Sensor ids descend while box ids ascend, so neither creation order
        // nor one id order matches the other.
        expect(scene.createEntityWithId(SceneEntityId{100 - zone}, "Sensor", sensor).success &&
                   scene.createEntityWithId(SceneEntityId{200 + zone}, "Box", box).success,
               "trigger order entities are created with their ids");
        scene.registry().get<TransformComponent>(sensor).local = glm::translate(glm::mat4(1.0f), center);
        scene.registry().emplace<ColliderComponent>(sensor, BoxCollider{glm::vec3(1.0f)});
        scene.registry().emplace<RigidBodyComponent>(sensor, RigidBodyComponent{.sensor = true});
        scene.registry().get<TransformComponent>(box).local = glm::translate(glm::mat4(1.0f), center);
        scene.registry().emplace<ColliderComponent>(box, BoxCollider{glm::vec3(0.25f)});
        scene.registry().emplace<RigidBodyComponent>(
            box, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .layer = PhysicsLayer::Dynamic});
    }
    std::vector<TriggerRecord> records;
    for (int step = 0; step < 90; ++step) {
        expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
               "the trigger order world steps");
        const std::span<const TriggerEvent> events = physics.triggerEvents();
        expect(std::ranges::is_sorted(events, triggerEventPrecedes), "a step's trigger events are in order");
        expect(std::ranges::is_sorted(physics.contactEvents(), contactEventPrecedes),
               "a step's contact events are in order");
        for (const TriggerEvent& event : events) {
            records.push_back({event.type, event.sensorId.value, event.otherId.value});
        }
    }
    physics.shutdown();
    return records;
}

void testSimultaneousTriggersArriveInOrder() {
    const std::array<uint32_t, 5> ascending{0, 1, 2, 3, 4};
    const std::array<uint32_t, 5> shuffled{3, 0, 4, 1, 2};
    const std::vector<TriggerRecord> first = simultaneousTriggerEvents(ascending);
    const std::vector<TriggerRecord> second = simultaneousTriggerEvents(shuffled);
    expect(first.size() == 10, "every box enters and leaves its sensor");
    expect(first.size() >= 5 && first[0] == TriggerRecord{TriggerEventType::Entered, 96, 204} &&
               first[4] == TriggerRecord{TriggerEventType::Entered, 100, 200},
           "simultaneous entries arrive by sensor scene id");
    expect(first == second, "the trigger sequence does not depend on creation order");
}
} // namespace

int main() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics world initializes");

    Scene scene;
    AssetManager assets;
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(5.0f, 0.5f, 5.0f));
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(1.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);

    const Entity box = scene.createEntity("Box");
    scene.registry().get<TransformComponent>(box).local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
    scene.registry().emplace<ColliderComponent>(box, BoxCollider{glm::vec3(0.5f)});
    RigidBodyComponent body;
    body.motionType = PhysicsMotionType::Dynamic;
    body.layer = PhysicsLayer::Dynamic;
    scene.registry().emplace<RigidBodyComponent>(box, body);

    bool sawContact = false;
    for (int i = 0; i < 180; ++i) {
        const PhysicsResult result = physics.step(scene, assets, physics.config().fixedTimeStep);
        expect(static_cast<bool>(result), "physics step succeeds");
        if (!result) {
            break;
        }
        sawContact |= !physics.contactEvents().empty();
    }
    const float settledHeight = scene.registry().get<TransformComponent>(box).local[3].y;
    expect(settledHeight > 0.45f && settledHeight < 0.65f, "dynamic box settles on the floor");
    expect(sawContact, "body contacts are exposed to gameplay code");

    PhysicsRayHit hit;
    expect(physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit), "raycast hits a physics body");
    expect(hit.entity == box || hit.entity == floor, "raycast resolves the hit to an ECS entity");

    expect(physics.applyImpulse(box, {2.0f, 3.0f, 0.0f}), "impulse applies to a managed body");
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
           "physics advances after an impulse");
    expect(physics.linearVelocity(box).x > 0.0f, "impulse changes linear velocity");

    const auto sample = assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
    expect(static_cast<bool>(sample), "mesh collider fixture loads");
    const ModelAsset* model = sample ? assets.model(sample.handle) : nullptr;
    if (model && !model->nodes.empty()) {
        const Entity meshEntity = scene.createEntity("Static Mesh");
        scene.registry().get<TransformComponent>(meshEntity).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, 3.0f));
        scene.registry().emplace<ColliderComponent>(meshEntity, TriangleMeshCollider{model->nodes.front().mesh});
        scene.registry().emplace<RigidBodyComponent>(meshEntity);
        expect(static_cast<bool>(physics.refreshBody(meshEntity, scene, assets)), "static triangle mesh body builds");
        PhysicsRayHit meshHit;
        expect(physics.raycast({3.0f, 4.0f, 3.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, meshHit) && meshHit.entity == meshEntity,
               "raycast hits a static triangle mesh");
    }

    const Entity lateBody = scene.createEntity("Late static body");
    scene.registry().emplace<ColliderComponent>(lateBody, BoxCollider{glm::vec3(0.25f)});
    scene.registry().emplace<RigidBodyComponent>(lateBody);
    scene.markPhysicsDirty(lateBody);
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
           "queued body creation succeeds without a reconciliation scan");
    expect(physics.stats().lifecycleOperations == 1 && physics.stats().auditedEntities == 0,
           "steady physics synchronization scales with lifecycle changes rather than total entities");
    expect(physics.stats().transformsReadBack <= physics.stats().activeBodies,
           "physics reads transforms back only for active dynamic bodies");

    scene.destroyEntity(box);
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
           "destroyed entity is removed from physics");
    expect(physics.stats().lifecycleOperations == 1 && physics.stats().auditedEntities == 0,
           "queued destruction avoids a full ECS reconciliation scan");
    physics.clear();
    PhysicsRayHit clearedHit;
    expect(!physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, clearedHit),
           "clearing physics removes all managed bodies");
    physics.shutdown();

    testWorldsShareJoltRuntime();
    testUnbuildableBodiesAreSkipped();
    testBodyCapacityIsClamped();
    testContactCapacityIsAWarning();
    testConfigurationLimitsAreValidated();
    testEventOrderIsByScene();
    testContactPoints();
    testSimultaneousTriggersArriveInOrder();
    return failures == 0 ? 0 : 1;
}
