// Navigation: baking a mesh from static colliders, paths around obstacles,
// partial paths to unreachable targets, agents walking through the crowd
// (avoiding each other, rejoining after a rebake, leaving when destroyed),
// the engine's agent system, and the session's bake policy.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/navigation/NavAgent.hpp"
#include "engine/navigation/NavigationCrowd.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/physics/CharacterController.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string>
#include <utility>
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
    expect(navigation.findPath({-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, path) == NavPath::None,
           "an unbaked world finds no path");
    const NavigationResult baked = navigation.bake(scene, assets);
    expect(baked.success && navigation.baked() && navigation.stats().polygons > 0,
           "a navigation mesh bakes from the static colliders");
    if (!baked) {
        std::cerr << baked.error << '\n';
        return;
    }

    expect(navigation.findPath({-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, path) == NavPath::Complete && path.size() >= 3,
           "a path crosses the wall's side");
    const bool aroundTheWall =
        std::ranges::any_of(path, [](const glm::vec3& corner) { return std::abs(corner.z) > 6.0f; });
    expect(aroundTheWall, "a path goes around the wall rather than through it");
    expect(!path.empty() && glm::distance(path.front(), glm::vec3(-4.0f, path.front().y, 0.0f)) < 0.3f &&
               glm::distance(path.back(), glm::vec3(4.0f, path.back().y, 0.0f)) < 0.3f,
           "a path starts and ends at its points");

    expect(navigation.findPath({-4.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, path) == NavPath::Partial && !path.empty() &&
               path.back().x > 9.0f && std::abs(path.back().z) < 1.0f,
           "a separate island is reached partially: the path ends at the floor's edge nearest it");
    expect(navigation.findPath({-4.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 100.0f}, path) == NavPath::None && path.empty(),
           "a point off the mesh has no path");
    const std::optional<glm::vec3> nearest = navigation.nearestPoint({0.0f, 0.0f, 0.0f});
    expect(nearest && std::abs(nearest->x) > 0.25f, "the nearest point to the wall's centre is beside it");

    DebugDraw draw;
    navigation.drawDebug(draw);
    expect(draw.lineCount() > 0, "the mesh draws its polygon edges");
    navigation.clear();
    expect(!navigation.baked(), "clearing removes the mesh");
    expect(!navigation.bake(NavigationGeometry{}).success, "baking nothing fails");
}

// Runs the agents for frames of seconds each.
void walk(Scene& scene, const NavigationWorld& navigation, NavigationCrowd& crowd, int frames, float seconds = 0.05f) {
    for (int frame = 0; frame < frames; ++frame) {
        updateNavAgents(scene, navigation, crowd, seconds);
        scene.updateWorldTransforms();
    }
}

glm::vec3 positionOf(const Scene& scene, Entity entity) {
    return glm::vec3(scene.registry().get<TransformComponent>(entity).world[3]);
}

Entity addWalker(Scene& scene, const glm::vec3& position, float speed = 3.5f) {
    const Entity walker = scene.createEntity("Walker");
    scene.registry().get<TransformComponent>(walker).local = glm::translate(glm::mat4(1.0f), position);
    scene.registry().emplace<NavAgentComponent>(walker, NavAgentComponent{.speed = speed});
    scene.updateWorldTransforms();
    return walker;
}

void testAgents() {
    Scene scene;
    buildLevel(scene);
    AssetManager assets;
    NavigationWorld navigation;
    navigation.bake(scene, assets);
    NavigationCrowd crowd;

    const Entity walker = scene.createEntity("Walker");
    scene.registry().get<TransformComponent>(walker).local = glm::translate(glm::mat4(1.0f), {-4.0f, 0.0f, 0.0f});
    auto& agent = scene.registry().emplace<NavAgentComponent>(walker, NavAgentComponent{.speed = 5.0f});
    setNavDestination(agent, {4.0f, 0.0f, 0.0f});
    scene.updateWorldTransforms();
    for (int frame = 0; frame < 200 && !scene.registry().get<NavAgentComponent>(walker).arrived; ++frame) {
        walk(scene, navigation, crowd, 1);
    }
    const glm::vec3 position(scene.registry().get<TransformComponent>(walker).world[3]);
    const NavAgentComponent& arrived = scene.registry().get<NavAgentComponent>(walker);
    expect(arrived.arrived && !arrived.destination && glm::distance(position, glm::vec3(4.0f, position.y, 0.0f)) < 0.3f,
           "an agent walks around the wall to its destination and stops");

    setNavDestination(scene.registry().get<NavAgentComponent>(walker), {30.0f, 0.0f, 0.0f});
    for (int frame = 0; frame < 200 && !scene.registry().get<NavAgentComponent>(walker).arrived; ++frame) {
        walk(scene, navigation, crowd, 1);
    }
    const NavAgentComponent& nearest = scene.registry().get<NavAgentComponent>(walker);
    expect(nearest.arrived && nearest.partialPath && !nearest.pathFailed && positionOf(scene, walker).x > 8.5f,
           "an agent sent to an unreachable island walks as near as it can and arrives there");

    setNavDestination(scene.registry().get<NavAgentComponent>(walker), {100.0f, 0.0f, 100.0f});
    walk(scene, navigation, crowd, 1);
    expect(scene.registry().get<NavAgentComponent>(walker).pathFailed &&
               !scene.registry().get<NavAgentComponent>(walker).destination,
           "an agent sent off the mesh gives up");

    const Entity character = scene.createEntity("Character");
    scene.registry().get<TransformComponent>(character).local = glm::translate(glm::mat4(1.0f), {-4.0f, 0.9f, 2.0f});
    scene.registry().emplace<CharacterControllerComponent>(character, CharacterControllerComponent{.moveAction = ""});
    setNavDestination(scene.registry().emplace<NavAgentComponent>(character, NavAgentComponent{.speed = 3.0f}),
                      {-4.0f, 0.0f, 8.0f});
    scene.updateWorldTransforms();
    walk(scene, navigation, crowd, 1);
    const glm::vec3 desired = scene.registry().get<CharacterControllerComponent>(character).desiredVelocity;
    expect(std::abs(glm::length(desired) - 3.0f) < 0.3f && desired.z > 2.5f && desired.y == 0.0f,
           "an agent with a character controller steers its desired velocity");
}

// Two agents walking head-on along the same line pass each other instead of
// walking through each other, and both arrive.
void testAvoidance() {
    Scene scene;
    buildLevel(scene);
    AssetManager assets;
    NavigationWorld navigation;
    navigation.bake(scene, assets);
    NavigationCrowd crowd;
    const Entity east = addWalker(scene, {-6.0f, 0.0f, 8.0f});
    const Entity west = addWalker(scene, {6.0f, 0.0f, 8.0f});
    setNavDestination(scene.registry().get<NavAgentComponent>(east), {6.0f, 0.0f, 8.0f});
    setNavDestination(scene.registry().get<NavAgentComponent>(west), {-6.0f, 0.0f, 8.0f});
    float closest = 100.0f;
    for (int frame = 0; frame < 300; ++frame) {
        walk(scene, navigation, crowd, 1);
        const glm::vec3 offset = positionOf(scene, east) - positionOf(scene, west);
        closest = std::min(closest, glm::length(glm::vec2(offset.x, offset.z)));
        if (scene.registry().get<NavAgentComponent>(east).arrived &&
            scene.registry().get<NavAgentComponent>(west).arrived) {
            break;
        }
    }
    expect(closest > 0.6f, "agents walking head-on keep apart");
    expect(scene.registry().get<NavAgentComponent>(east).arrived &&
               scene.registry().get<NavAgentComponent>(west).arrived,
           "agents that avoided each other both arrive");
}

// The crowd follows the mesh and the scene: a rebake starts it over with the
// agents rejoining, destroyed agents leave it, a copied agent takes its own
// slot, and a full crowd turns agents away.
void testCrowdMembership() {
    Scene scene;
    buildLevel(scene);
    AssetManager assets;
    NavigationWorld navigation;
    navigation.bake(scene, assets);
    NavigationCrowd crowd;
    const Entity first = addWalker(scene, {-6.0f, 0.0f, -8.0f});
    const Entity second = addWalker(scene, {-6.0f, 0.0f, 8.0f});
    walk(scene, navigation, crowd, 1);
    expect(crowd.agentCount() == 2, "idle agents join the crowd, so others walk around them");

    const uint64_t generation = navigation.generation();
    navigation.bake(scene, assets);
    expect(navigation.generation() != generation, "a rebake changes the mesh generation");
    setNavDestination(scene.registry().get<NavAgentComponent>(first), {6.0f, 0.0f, -8.0f});
    walk(scene, navigation, crowd, 1);
    expect(crowd.meshGeneration() == navigation.generation() && crowd.agentCount() == 2,
           "after a rebake the crowd starts over and the agents rejoin");
    for (int frame = 0; frame < 200 && !scene.registry().get<NavAgentComponent>(first).arrived; ++frame) {
        walk(scene, navigation, crowd, 1);
    }
    expect(scene.registry().get<NavAgentComponent>(first).arrived, "an agent walks on the rebaked mesh");

    const Entity copy = scene.createEntity("Copy");
    scene.registry().get<TransformComponent>(copy).local = glm::translate(glm::mat4(1.0f), {-2.0f, 0.0f, 8.0f});
    scene.registry().emplace<NavAgentComponent>(copy, scene.registry().get<NavAgentComponent>(second));
    scene.updateWorldTransforms();
    walk(scene, navigation, crowd, 1);
    expect(crowd.agentCount() == 3 && scene.registry().get<NavAgentComponent>(copy).crowdAgent !=
                                          scene.registry().get<NavAgentComponent>(second).crowdAgent,
           "a copied agent takes its own slot instead of sharing its original's");

    scene.destroyEntity(second);
    walk(scene, navigation, crowd, 1);
    expect(crowd.agentCount() == 2, "a destroyed agent leaves the crowd");

    NavigationCrowd full;
    const NavCrowdSettings oneAgent{.maximumAgents = 1};
    Scene crowded;
    buildLevel(crowded);
    // Which agent the crowd admits first follows the ECS view's order.
    const Entity one = addWalker(crowded, {-6.0f, 0.0f, -8.0f});
    const Entity other = addWalker(crowded, {-6.0f, 0.0f, 8.0f});
    setNavDestination(crowded.registry().get<NavAgentComponent>(one), {-6.0f, 0.0f, -4.0f});
    setNavDestination(crowded.registry().get<NavAgentComponent>(other), {-6.0f, 0.0f, 4.0f});
    updateNavAgents(crowded, navigation, full, 0.05f, oneAgent);
    const bool oneFailed = crowded.registry().get<NavAgentComponent>(one).pathFailed;
    const bool otherFailed = crowded.registry().get<NavAgentComponent>(other).pathFailed;
    expect(full.agentCount() == 1 && oneFailed != otherFailed, "an agent beyond the crowd's maximum is turned away");
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

// Runs the engine's Update systems (zenith.nav_agents among them) for frames.
void runAgentFrames(Scene& scene, GameServices& services, Logger& logger, int frames) {
    const Gameplay gameplay = builtInGameplay();
    for (int frame = 0; frame < frames; ++frame) {
        gameplay.systems->run(SystemPhase::Update, {.scene = scene,
                                                    .mode = SimulationMode::Play,
                                                    .deltaSeconds = 0.1f,
                                                    .logger = &logger,
                                                    .services = &services});
        scene.updateWorldTransforms();
    }
}

size_t waitingWarnings(const Logger& logger) {
    const std::vector<LogEntry> entries = logger.entries();
    return static_cast<size_t>(std::ranges::count_if(entries, [](const LogEntry& entry) {
        return entry.level == LogLevel::Warning && entry.message.find("wait for") != std::string::npos;
    }));
}

// A generated level spawns its enemies before it bakes. Under the Lazy policy
// the agent system bakes the half-built level first, so the game's own bake is
// a second one; under Explicit only the game's bake runs, and agents wait for
// it with their destinations.
void testBakePolicy() {
    for (const NavigationBakePolicy policy : {NavigationBakePolicy::Lazy, NavigationBakePolicy::Explicit}) {
        const bool lazy = policy == NavigationBakePolicy::Lazy;
        Scene scene;
        buildLevel(scene);
        Logger logger(LoggerConfig{.echoToStderr = false});
        AssetManager assets{&logger};
        DebugDraw debugDraw;
        GameServices services(assets, {}, debugDraw, &logger);
        expect(services.navigationBakePolicy() == NavigationBakePolicy::Lazy, "sessions bake lazily by default");
        services.setNavigationBakePolicy(policy);
        const Entity walker = addWalker(scene, {-4.0f, 0.0f, -8.0f});
        setNavDestination(scene.registry().get<NavAgentComponent>(walker), {4.0f, 0.0f, -8.0f});
        // Asking navigation() would bake under Lazy, so start from a new world.
        const uint64_t unbaked = NavigationWorld{}.generation();

        runAgentFrames(scene, services, logger, 3);
        const NavAgentComponent& agent = scene.registry().get<NavAgentComponent>(walker);
        if (lazy) {
            expect(services.navigation(scene).generation() == unbaked + 1,
                   "the Lazy policy bakes as soon as an agent exists");
        } else {
            expect(services.navigation(scene).generation() == unbaked && !services.navigation(scene).baked(),
                   "the Explicit policy never bakes on its own");
            expect(agent.destination.has_value() && !agent.pathFailed && positionOf(scene, walker).x < -3.9f,
                   "agents wait for the bake with their destinations");
            expect(waitingWarnings(logger) == 1, "waiting agents are reported once");
        }

        expect(services.bakeNavigation(scene).success, "the game bakes its level");
        expect(services.navigation(scene).generation() == unbaked + (lazy ? 2u : 1u),
               lazy ? "the Lazy policy bakes a level the game bakes too twice"
                    : "the Explicit policy bakes a level the game bakes once");
        runAgentFrames(scene, services, logger, 5);
        expect(positionOf(scene, walker).x > -3.0f, "agents walk once the mesh is baked");
    }
}

// A host's default settings (the project's) are what the lazy bake and
// bakeNavigation without settings use; navigationIfBaked only looks.
void testDefaultSettingsAndLooking() {
    Scene scene;
    buildLevel(scene);
    Logger logger(LoggerConfig{.echoToStderr = false});
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    GameServices services(assets, {}, debugDraw, &logger);
    const NavMeshBuildSettings project{.cellSize = 0.25f, .agentRadius = 0.8f};
    expect(services.defaultNavigationSettings() == NavMeshBuildSettings{}, "sessions start with the default settings");
    services.setDefaultNavigationSettings(project);

    expect(services.navigationIfBaked() == nullptr, "nothing is baked before anything asks");
    expect(services.navigationIfBaked() == nullptr && std::as_const(services).navigationCrowd().agentCount() == 0,
           "looking at the mesh and crowd never bakes");
    const NavigationWorld& lazy = services.navigation(scene);
    expect(lazy.baked() && lazy.settings() == project, "the lazy bake uses the session's default settings");
    expect(services.navigationIfBaked() == &lazy, "once baked, the mesh can be looked at");

    expect(services.bakeNavigation(scene).success && services.navigation(scene).settings() == project,
           "bakeNavigation without settings uses the defaults");
    const NavMeshBuildSettings explicitSettings{.agentRadius = 0.2f};
    expect(services.bakeNavigation(scene, explicitSettings).success &&
               services.navigation(scene).settings() == explicitSettings,
           "settings passed to bakeNavigation win");
}
} // namespace

// Geometry gathered right after a move, with no updateWorldTransforms in
// between, sits where the hierarchy now places it.
void testGeometryBeforeTransformUpdate() {
    Scene scene;
    const Entity level = scene.createEntity("Level");
    const Entity floor = addBox(scene, {0.0f, -0.5f, 0.0f}, {2.0f, 0.5f, 2.0f});
    scene.setParent(floor, level);
    scene.updateWorldTransforms();
    scene.setPosition(level, {50.0f, 0.0f, 0.0f});

    AssetManager assets;
    const NavigationGeometry geometry = gatherNavigationGeometry(scene, assets);
    expect(!geometry.vertices.empty() && std::all_of(geometry.vertices.begin(), geometry.vertices.end(),
                                                     [](const glm::vec3& vertex) { return vertex.x >= 47.9f; }),
           "geometry follows a moved parent before world transforms update");

    NavigationWorld navigation;
    expect(navigation.bake(scene, assets).success, "a bake straight after the move succeeds");
    const std::optional<glm::vec3> onMesh = navigation.nearestPoint({50.0f, 0.0f, 0.0f});
    expect(onMesh && glm::distance(*onMesh, glm::vec3(50.0f, 0.0f, 0.0f)) < 0.5f,
           "the baked mesh lies under the moved floor");
}

int main() {
    testGeometry();
    testGeometryBeforeTransformUpdate();
    testBakeAndPaths();
    testAgents();
    testAvoidance();
    testCrowdMembership();
    testEngineSystem();
    testBakePolicy();
    testDefaultSettingsAndLooking();
    return failures == 0 ? 0 : 1;
}
