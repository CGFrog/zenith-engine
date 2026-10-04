// Navigation: baking a mesh from static colliders, paths around obstacles,
// unreachable targets, agents following paths, and the engine's agent system.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/navigation/NavAgent.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/physics/CharacterController.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

Entity addBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents, RigidBodyComponent body = {}) {
    const Entity entity = scene.createEntity("Box");
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), center);
    scene.registry().emplace<ColliderComponent>(entity, BoxCollider{halfExtents});
    scene.registry().emplace<RigidBodyComponent>(entity, body);
    return entity;
}

// A 20 m floor (top at y = 0) split by a wall along z from -6 to 6 at x = 0,
// and a separate island at x = 30.
void buildLevel(Scene& scene) {
    addBox(scene, {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f});
    addBox(scene, {0.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 6.0f});
    addBox(scene, {30.0f, -0.5f, 0.0f}, {3.0f, 0.5f, 3.0f});
    scene.updateWorldTransforms();
}

void testGeometry() {
    Scene scene;
    addBox(scene, {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f});
    addBox(scene, {0.0f, 3.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {.motionType = PhysicsMotionType::Dynamic});
    addBox(scene, {4.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {.sensor = true});
    addBox(scene, {-4.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {.layer = PhysicsLayer::Player});
    scene.updateWorldTransforms();
    AssetManager assets;
    const NavigationGeometry all = gatherNavigationGeometry(scene, assets);
    expect(all.indices.size() == 2 * 12 * 3 && all.vertices.size() == 2 * 24,
           "only static, solid colliders become navigation geometry, a box as twelve triangles");
    const NavigationGeometry masked = gatherNavigationGeometry(scene, assets, physicsLayerMask({PhysicsLayer::Static}));
    expect(masked.indices.size() == 12 * 3, "the layer mask chooses which colliders count");
    // The floor's top face winds upward, as Recast expects of walkable ground.
    bool upward = false;
    for (size_t index = 0; index + 2 < all.indices.size(); index += 3) {
        const glm::vec3 a = all.vertices[all.indices[index]];
        const glm::vec3 b = all.vertices[all.indices[index + 1]];
        const glm::vec3 c = all.vertices[all.indices[index + 2]];
        upward = upward || (a.y == 0.0f && b.y == 0.0f && c.y == 0.0f && glm::cross(b - a, c - a).y > 0.0f);
    }
    expect(upward, "upward faces wind counter-clockwise seen from above");
}

void testBakeAndPaths() {
    Scene scene;
    buildLevel(scene);
    AssetManager assets;
    NavigationWorld navigation;
    std::vector<glm::vec3> path;
    expect(!navigation.findPath({-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, path), "an unbaked world finds no path");
    const NavigationResult baked = navigation.bake(scene, assets);
    expect(baked.success && navigation.baked() && navigation.stats().polygons > 0,
           "a navigation mesh bakes from the static colliders");
    if (!baked) {
        std::cerr << baked.error << '\n';
        return;
    }

    expect(navigation.findPath({-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, path) && path.size() >= 3,
           "a path crosses the wall's side");
    const bool aroundTheWall =
        std::ranges::any_of(path, [](const glm::vec3& corner) { return std::abs(corner.z) > 6.0f; });
    expect(aroundTheWall, "a path goes around the wall rather than through it");
    expect(!path.empty() && glm::distance(path.front(), glm::vec3(-4.0f, path.front().y, 0.0f)) < 0.3f &&
               glm::distance(path.back(), glm::vec3(4.0f, path.back().y, 0.0f)) < 0.3f,
           "a path starts and ends at its points");

    expect(!navigation.findPath({-4.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, path) && path.empty(),
           "a separate island cannot be reached");
    expect(!navigation.findPath({-4.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 100.0f}, path),
           "a point off the mesh cannot be reached");
    const std::optional<glm::vec3> nearest = navigation.nearestPoint({0.0f, 0.0f, 0.0f});
    expect(nearest && std::abs(nearest->x) > 0.25f, "the nearest point to the wall's centre is beside it");

    DebugDraw draw;
    navigation.drawDebug(draw);
    expect(draw.lineCount() > 0, "the mesh draws its polygon edges");
    navigation.clear();
    expect(!navigation.baked(), "clearing removes the mesh");
    expect(!navigation.bake(NavigationGeometry{}).success, "baking nothing fails");
}

void testAgents() {
    Scene scene;
    buildLevel(scene);
    AssetManager assets;
    NavigationWorld navigation;
    navigation.bake(scene, assets);

    const Entity walker = scene.createEntity("Walker");
    scene.registry().get<TransformComponent>(walker).local = glm::translate(glm::mat4(1.0f), {-4.0f, 0.0f, 0.0f});
    auto& agent = scene.registry().emplace<NavAgentComponent>(walker, NavAgentComponent{.speed = 5.0f});
    setNavDestination(agent, {4.0f, 0.0f, 0.0f});
    scene.updateWorldTransforms();
    for (int frame = 0; frame < 200 && !scene.registry().get<NavAgentComponent>(walker).arrived; ++frame) {
        updateNavAgents(scene, navigation, 0.05f);
        scene.updateWorldTransforms();
    }
    const glm::vec3 position(scene.registry().get<TransformComponent>(walker).world[3]);
    const NavAgentComponent& arrived = scene.registry().get<NavAgentComponent>(walker);
    expect(arrived.arrived && !arrived.destination && glm::distance(position, glm::vec3(4.0f, position.y, 0.0f)) < 0.3f,
           "an agent walks around the wall to its destination and stops");

    setNavDestination(scene.registry().get<NavAgentComponent>(walker), {30.0f, 0.0f, 0.0f});
    updateNavAgents(scene, navigation, 0.05f);
    expect(scene.registry().get<NavAgentComponent>(walker).pathFailed &&
               !scene.registry().get<NavAgentComponent>(walker).destination,
           "an agent with an unreachable destination gives up");

    const Entity character = scene.createEntity("Character");
    scene.registry().get<TransformComponent>(character).local = glm::translate(glm::mat4(1.0f), {-4.0f, 0.9f, 2.0f});
    scene.registry().emplace<CharacterControllerComponent>(character, CharacterControllerComponent{.moveAction = ""});
    setNavDestination(scene.registry().emplace<NavAgentComponent>(character, NavAgentComponent{.speed = 3.0f}),
                      {-4.0f, 0.0f, 8.0f});
    scene.updateWorldTransforms();
    updateNavAgents(scene, navigation, 0.05f);
    const glm::vec3 desired = scene.registry().get<CharacterControllerComponent>(character).desiredVelocity;
    expect(std::abs(glm::length(desired) - 3.0f) < 0.01f && desired.z > 2.9f && desired.y == 0.0f,
           "an agent with a character controller steers its desired velocity");
}

void testEngineSystem() {
    Scene scene;
    buildLevel(scene);
    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    GameServices services(assets, {}, debugDraw, &logger);
    const Entity walker = scene.createEntity("Walker");
    scene.registry().get<TransformComponent>(walker).local = glm::translate(glm::mat4(1.0f), {-4.0f, 0.0f, -8.0f});
    setNavDestination(scene.registry().emplace<NavAgentComponent>(walker), {4.0f, 0.0f, -8.0f});
    scene.updateWorldTransforms();

    const Gameplay gameplay = builtInGameplay();
    for (int frame = 0; frame < 5; ++frame) {
        gameplay.systems->run(SystemPhase::Update, {.scene = scene,
                                                    .mode = SimulationMode::Play,
                                                    .deltaSeconds = 0.1f,
                                                    .logger = &logger,
                                                    .services = &services});
        scene.updateWorldTransforms();
    }
    expect(services.navigation(scene).baked(), "the session bakes its navigation mesh when an agent needs it");
    expect(scene.registry().get<TransformComponent>(walker).world[3].x > -3.0f,
           "the engine's agent system moves agents in Play");
}
} // namespace

int main() {
    testGeometry();
    testBakeAndPaths();
    testAgents();
    testEngineSystem();
    return failures == 0 ? 0 : 1;
}
