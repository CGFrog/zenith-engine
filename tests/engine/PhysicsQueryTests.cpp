// Physics queries with filters, project collision layers, sensor trigger
// events, and the rigid body's layer field across schema versions.

#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
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

bool near(float value, float expected, float tolerance = 0.02f) {
    return std::abs(value - expected) <= tolerance;
}

struct BodyDescription {
    glm::vec3 position{0.0f};
    ColliderShape shape{BoxCollider{}};
    PhysicsMotionType motion{PhysicsMotionType::Static};
    PhysicsLayer layer{PhysicsLayer::Static};
    bool sensor{false};
    glm::vec3 scale{1.0f};
};

Entity addBody(Scene& scene, std::string name, const BodyDescription& body) {
    const Entity entity = scene.createEntity(std::move(name));
    scene.registry().get<TransformComponent>(entity).local =
        glm::translate(glm::mat4(1.0f), body.position) * glm::scale(glm::mat4(1.0f), body.scale);
    scene.registry().emplace<ColliderComponent>(entity, body.shape);
    scene.registry().emplace<RigidBodyComponent>(
        entity, RigidBodyComponent{.motionType = body.motion, .layer = body.layer, .sensor = body.sensor});
    return entity;
}

// A floor whose top is at y = 0, ten metres square.
Entity addFloor(Scene& scene) {
    return addBody(scene, "Floor",
                   {.position = {0.0f, -0.5f, 0.0f}, .shape = BoxCollider{glm::vec3(5.0f, 0.5f, 5.0f)}});
}

void step(PhysicsWorld& physics, Scene& scene, const AssetManager& assets, int steps = 1) {
    for (int index = 0; index < steps; ++index) {
        expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)), "physics steps");
    }
}

void testRaysAndFilters() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    Scene scene;
    AssetManager assets;
    const Entity floor = addFloor(scene);
    // A capsule from y = 0.2 to y = 1.8, as a player would be.
    const Entity player = addBody(scene, "Player",
                                  {.position = {0.0f, 1.0f, 0.0f},
                                   .shape = CapsuleCollider{0.5f, 0.3f},
                                   .motion = PhysicsMotionType::Kinematic,
                                   .layer = PhysicsLayer::Player});
    const Entity sensor = addBody(
        scene, "Sensor", {.position = {-3.0f, 1.0f, 0.0f}, .shape = BoxCollider{glm::vec3(0.5f)}, .sensor = true});
    step(physics, scene, assets);

    PhysicsRayHit hit;
    expect(physics.raycast({0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) && hit.entity == player,
           "a ray from inside its owner hits the owner");
    const Entity owner[] = {player};
    expect(physics.raycast({0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit, {.ignore = owner}) &&
               hit.entity == floor && near(hit.position.y, 0.0f) && near(hit.normal.y, 1.0f),
           "a ray that ignores its owner hits what lies beyond");
    expect(!physics.raycast({0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit,
                            {.layerMask = physicsLayerMask({PhysicsLayer::Dynamic})}),
           "a layer mask leaves out the other layers");
    expect(physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit,
                           {.layerMask = physicsLayerMask({PhysicsLayer::Static})}) &&
               hit.entity == floor,
           "a layer mask lets a ray pass through other layers' bodies");

    expect(physics.raycast({-3.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) && hit.entity == floor,
           "rays pass through sensors by default");
    expect(physics.raycast({-3.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit, {.hitSensors = true}) &&
               hit.entity == sensor,
           "a ray asked to can hit sensors");

    std::vector<PhysicsRayHit> hits;
    expect(physics.raycastAll({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hits) && hits.size() == 2 &&
               hits[0].entity == player && hits[1].entity == floor && hits[0].fraction < hits[1].fraction &&
               near(hits[0].position.y, 1.8f) && near(hits[1].position.y, 0.0f),
           "raycastAll returns every body along the ray, nearest first");
    expect(!physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 10.0f, hit) &&
               !physics.raycastAll({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.0f, hits) && hits.empty(),
           "rays without a direction or length hit nothing");
}

void testShapeQueries() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    Scene scene;
    AssetManager assets;
    const Entity floor = addFloor(scene);
    const Entity player = addBody(scene, "Player",
                                  {.position = {0.0f, 1.0f, 0.0f},
                                   .shape = CapsuleCollider{0.5f, 0.3f},
                                   .motion = PhysicsMotionType::Kinematic,
                                   .layer = PhysicsLayer::Player});
    step(physics, scene, assets);

    PhysicsShapeHit hit;
    expect(physics.shapeCast(PhysicsQuerySphere{0.25f}, {.position = {3.0f, 3.0f, 0.0f}}, {0.0f, -1.0f, 0.0f}, 10.0f,
                             hit) &&
               hit.entity == floor && near(hit.fraction, 0.275f) && near(hit.position.y, 0.0f) &&
               near(hit.normal.y, 1.0f, 0.05f),
           "a sphere cast stops on the floor, reporting where and the floor's normal");
    expect(!physics.shapeCast(PhysicsQueryCapsule{0.5f, 0.25f}, {.position = {3.0f, 3.0f, 0.0f}}, {0.0f, 1.0f, 0.0f},
                              10.0f, hit),
           "a capsule cast away from every body misses");
    expect(physics.shapeCast(PhysicsQueryBox{glm::vec3(0.2f)}, {.position = {-2.0f, 1.0f, 0.0f}}, {1.0f, 0.0f, 0.0f},
                             5.0f, hit) &&
               hit.entity == player && near(hit.fraction, 0.3f),
           "a box cast meets the first body in its path");
    const Entity ignored[] = {player};
    expect(!physics.shapeCast(PhysicsQueryBox{glm::vec3(0.2f)}, {.position = {-2.0f, 1.0f, 0.0f}}, {1.0f, 0.0f, 0.0f},
                              5.0f, hit, {.ignore = ignored}),
           "shape casts honour the filter");
    expect(!physics.shapeCast(PhysicsQuerySphere{0.0f}, {}, {0.0f, -1.0f, 0.0f}, 1.0f, hit),
           "a shape without size casts nothing");

    std::vector<Entity> overlapping;
    expect(physics.overlap(PhysicsQueryBox{glm::vec3(0.5f)}, {.position = {0.0f, 0.2f, 0.0f}}, overlapping) &&
               overlapping.size() == 2 && std::ranges::find(overlapping, floor) != overlapping.end() &&
               std::ranges::find(overlapping, player) != overlapping.end(),
           "overlap returns every body the shape touches, once each");
    expect(physics.overlap(PhysicsQueryBox{glm::vec3(0.5f)}, {.position = {0.0f, 0.2f, 0.0f}}, overlapping,
                           {.layerMask = physicsLayerMask({PhysicsLayer::Static})}) &&
               overlapping == std::vector<Entity>{floor},
           "overlap honours the layer mask");
    expect(!physics.overlap(PhysicsQuerySphere{0.5f}, {.position = {0.0f, 10.0f, 0.0f}}, overlapping) &&
               overlapping.empty(),
           "overlap in empty space finds nothing");

    PhysicsClosestPoint closest;
    expect(physics.closestPoint({3.0f, 2.0f, 0.0f}, 5.0f, closest) && closest.entity == floor &&
               near(closest.distance, 2.0f) && near(closest.position.y, 0.0f) && near(closest.position.x, 3.0f),
           "closestPoint finds the nearest surface and its distance");
    expect(physics.closestPoint({0.6f, 1.0f, 0.0f}, 5.0f, closest) && closest.entity == player &&
               near(closest.distance, 0.3f),
           "closestPoint prefers the nearest of several bodies");
    expect(!physics.closestPoint({3.0f, 2.0f, 0.0f}, 1.0f, closest), "closestPoint ignores bodies beyond its reach");
}

void testLayers() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    PhysicsLayerTable invalid = PhysicsLayerTable::defaults();
    invalid.names.push_back("Static");
    expect(!physics.setLayers(invalid), "a table naming a layer twice is refused");
    PhysicsWorld refused;
    expect(!refused.init({.layers = invalid}), "a world will not start with an invalid table");

    Scene scene;
    AssetManager assets;
    addFloor(scene);
    step(physics, scene, assets);
    PhysicsLayerTable table = PhysicsLayerTable::defaults();
    const std::optional<PhysicsLayer> ghost = table.add("Ghost");
    expect(ghost.has_value() && !physics.setLayers(table), "layers cannot change while bodies exist");
    physics.clear();
    expect(static_cast<bool>(physics.setLayers(table)), "layers change in an empty world");

    // Ghosts collide only with each other, so a ghost falls through the floor.
    table.setCollides(*ghost, *ghost, true);
    physics.clear();
    expect(static_cast<bool>(physics.setLayers(table)) && !table.collides(*ghost, PhysicsLayer::Static),
           "a custom matrix is accepted");
    const BodyDescription falling{.position = {0.0f, 2.0f, 0.0f},
                                  .shape = BoxCollider{glm::vec3(0.25f)},
                                  .motion = PhysicsMotionType::Dynamic,
                                  .layer = PhysicsLayer::Dynamic};
    BodyDescription ghostBody = falling;
    ghostBody.position.x = 2.0f;
    ghostBody.layer = *ghost;
    BodyDescription unknownBody = falling;
    unknownBody.position.x = -2.0f;
    unknownBody.layer = {20};
    const Entity solid = addBody(scene, "Solid", falling);
    const Entity phantom = addBody(scene, "Ghost", ghostBody);
    const Entity unknown = addBody(scene, "Unknown layer", unknownBody);
    step(physics, scene, assets, 120);
    const auto height = [&scene](Entity entity) { return scene.registry().get<TransformComponent>(entity).local[3].y; };
    expect(near(height(solid), 0.25f, 0.05f), "a Dynamic body rests on the Static floor");
    expect(height(phantom) < -1.0f, "a layer that does not collide with Static falls through the floor");
    expect(near(height(unknown), 0.25f, 0.05f), "a body on a layer the table lacks behaves as Dynamic");
}

struct TriggerLog {
    std::vector<TriggerEvent> events;

    void record(const PhysicsWorld& physics) {
        events.insert(events.end(), physics.triggerEvents().begin(), physics.triggerEvents().end());
    }

    size_t count(TriggerEventType type, Entity other) const {
        return std::ranges::count_if(
            events, [&](const TriggerEvent& event) { return event.type == type && event.other == other; });
    }
};

void testTriggers() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const BodyDescription ball{.position = {0.0f, 3.0f, 0.0f},
                               .shape = SphereCollider{0.2f},
                               .motion = PhysicsMotionType::Dynamic,
                               .layer = PhysicsLayer::Dynamic};
    // A thin slab the first ball falls through, and a box around where the
    // second ball comes to rest.
    const Entity slab =
        addBody(scene, "Slab sensor",
                {.position = {0.0f, 1.0f, 0.0f}, .shape = BoxCollider{glm::vec3(1.0f, 0.25f, 1.0f)}, .sensor = true});
    const Entity zone = addBody(
        scene, "Zone sensor", {.position = {3.0f, 1.0f, 0.0f}, .shape = BoxCollider{glm::vec3(1.0f)}, .sensor = true});
    const Entity through = addBody(scene, "Falling through", ball);
    BodyDescription resting = ball;
    resting.position.x = 3.0f;
    const Entity stays = addBody(scene, "Resting inside", resting);

    TriggerLog log;
    for (int index = 0; index < 120; ++index) {
        step(physics, scene, assets);
        log.record(physics);
    }
    expect(log.count(TriggerEventType::Entered, through) == 1 && log.count(TriggerEventType::Exited, through) == 1,
           "a body passing through a sensor enters and exits once");
    const auto order = [&log](Entity other) {
        std::vector<TriggerEventType> types;
        for (const TriggerEvent& event : log.events) {
            if (event.other == other) {
                types.push_back(event.type);
            }
        }
        return types;
    };
    expect(order(through) == std::vector<TriggerEventType>{TriggerEventType::Entered, TriggerEventType::Exited},
           "enter comes before exit");
    expect(log.events.front().sensor == slab || log.events.front().sensor == zone,
           "events name the sensor and the other body");
    // Jolt drops the contacts of a body that falls asleep; it is still inside.
    expect(physics.stats().activeBodies == 0, "both balls have come to rest and fallen asleep");
    expect(log.count(TriggerEventType::Entered, stays) == 1 && log.count(TriggerEventType::Exited, stays) == 0,
           "a body resting (and sleeping) in a sensor enters once and stays without more events");

    // Woken and knocked out of the sensor, it exits once and enters nothing.
    log.events.clear();
    expect(physics.applyImpulse(stays, {1.5f, 0.6f, 0.0f}), "the resting ball is knocked sideways");
    for (int index = 0; index < 90; ++index) {
        step(physics, scene, assets);
        log.record(physics);
    }
    expect(scene.registry().get<TransformComponent>(stays).local[3].x > 4.2f, "the knocked ball left the zone");
    expect(log.count(TriggerEventType::Exited, stays) == 1 && log.count(TriggerEventType::Entered, stays) == 0,
           "a body woken inside a sensor does not enter again, and exits when it leaves");

    // A body destroyed while inside.
    BodyDescription inZone = resting;
    inZone.position.y = 1.0f;
    const Entity doomed = addBody(scene, "Doomed", inZone);
    const SceneEntityId doomedId = scene.id(doomed);
    scene.markPhysicsDirty(doomed);
    log.events.clear();
    for (int index = 0; index < 10; ++index) {
        step(physics, scene, assets);
        log.record(physics);
    }
    expect(log.count(TriggerEventType::Entered, doomed) == 1, "a body created inside a sensor enters it");
    log.events.clear();
    scene.destroyEntity(doomed);
    step(physics, scene, assets);
    log.record(physics);
    expect(log.events.size() == 1 && log.events[0].type == TriggerEventType::Exited && log.events[0].sensor == zone &&
               log.events[0].otherId == doomedId && log.events[0].sensorId == scene.id(zone),
           "destroying a body inside a sensor reports its exit, by scene id");

    // The sensor itself goes away while a body is inside.
    const Entity inside = addBody(scene, "Inside", inZone);
    scene.markPhysicsDirty(inside);
    log.events.clear();
    for (int index = 0; index < 10; ++index) {
        step(physics, scene, assets);
        log.record(physics);
    }
    expect(log.count(TriggerEventType::Entered, inside) == 1, "a second body enters the sensor");
    log.events.clear();
    scene.destroyEntity(zone);
    step(physics, scene, assets);
    log.record(physics);
    expect(log.count(TriggerEventType::Exited, inside) == 1, "destroying a sensor reports the exit of what was inside");

    log.events.clear();
    physics.clear();
    expect(physics.triggerEvents().empty(), "clearing the world drops trigger state without events");
}

void testLayerFieldSchema() {
    Scene scene;
    AssetManager assets;
    const Entity body = addBody(scene, "Body", {.layer = {7}});
    std::string document;
    expect(SceneDocument::serialize(scene, assets, {}, document).success, "a body on a project layer saves");
    nlohmann::json json = nlohmann::json::parse(document);
    nlohmann::json* rigidBody = nullptr;
    for (nlohmann::json& entity : json["entities"]) {
        if (entity["components"].contains("rigid_body")) {
            rigidBody = &entity["components"]["rigid_body"];
        }
    }
    expect(rigidBody != nullptr && (*rigidBody)["layer"] == 7 && (*rigidBody)["$version"] == 2,
           "the layer is saved as its index, at schema version 2");
    if (rigidBody == nullptr) {
        return;
    }
    Scene loaded;
    expect(SceneDocument::deserialize(document, assets, {}, loaded).success &&
               loaded.registry().get<RigidBodyComponent>(loaded.findEntity(scene.id(body))).layer == PhysicsLayer{7},
           "a saved project layer loads");

    // Written before named layers: no version, one of the three built-ins.
    rigidBody->erase("$version");
    (*rigidBody)["layer"] = 2;
    Scene legacy;
    expect(SceneDocument::deserialize(json.dump(), assets, {}, legacy).success &&
               legacy.registry().get<RigidBodyComponent>(legacy.findEntity(scene.id(body))).layer ==
                   PhysicsLayer::Player,
           "a version 1 rigid body keeps its built-in layer");

    (*rigidBody)["layer"] = maxPhysicsLayers;
    Scene beyond;
    expect(!SceneDocument::deserialize(json.dump(), assets, {}, beyond).success,
           "a layer beyond the last one is refused");
}
} // namespace

int main() {
    testRaysAndFilters();
    testShapeQueries();
    testLayers();
    testTriggers();
    testLayerFieldSchema();
    if (failures == 0) {
        std::cout << "Physics query tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
