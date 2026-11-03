// The engine character controller: steps, slopes, moving platforms, jumps,
// the ground probe, how queries and sensors see it, and its component's
// schema migration.

#include "engine/audio/AudioSystem.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_BEGIN("CharacterController");

namespace {

bool near(float value, float expected, float tolerance) {
    return std::abs(value - expected) <= tolerance;
}

// The default capsule: its centre stands 0.9 m above the ground.
constexpr float standingHeight = 0.9f;

Entity addBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents,
              PhysicsMotionType motion = PhysicsMotionType::Static, float rollRadians = 0.0f) {
    const Entity entity = scene.createEntity("Box");
    scene.registry().get<TransformComponent>(entity).local =
        glm::translate(glm::mat4(1.0f), center) *
        glm::rotate(glm::mat4(1.0f), rollRadians, glm::vec3(0.0f, 0.0f, 1.0f));
    scene.registry().emplace<ColliderComponent>(entity, BoxCollider{halfExtents});
    scene.registry().emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = motion});
    return entity;
}

// A floor whose top is at y = 0, forty metres square.
Entity addFloor(Scene& scene) {
    return addBox(scene, {0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
}

Entity addCharacter(Scene& scene, const glm::vec3& position, CharacterControllerComponent controller = {}) {
    const Entity entity = scene.createEntity("Character");
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.registry().emplace<CharacterControllerComponent>(entity, controller);
    return entity;
}

glm::vec3 positionOf(const Scene& scene, Entity entity) {
    return glm::vec3(scene.registry().get<TransformComponent>(entity).world[3]);
}

CharacterControllerComponent& controllerOf(Scene& scene, Entity entity) {
    return scene.registry().get<CharacterControllerComponent>(entity);
}

void step(PhysicsWorld& physics, Scene& scene, const AssetManager& assets, int steps = 1) {
    for (int index = 0; index < steps; ++index) {
        const PhysicsResult result = physics.step(scene, assets, physics.config().fixedTimeStep);
        CHECK_MESSAGE(result.success, "physics steps");
        if (!result) {
            std::cerr << result.error << '\n';
            return;
        }
    }
}

// Walks a character from the origin along +x at 2 m/s for three seconds,
// and returns where it ended up.
glm::vec3 walkInto(const std::function<void(Scene&)>& build, CharacterControllerComponent controller = {}) {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    build(scene);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f}, controller);
    step(physics, scene, assets, 30);
    controllerOf(scene, character).desiredVelocity = {2.0f, 0.0f, 0.0f};
    step(physics, scene, assets, 180);
    return positionOf(scene, character);
}

TEST_CASE("CharacterController: steps") {
    // A 0.25 m step starting at x = 1.5, within the default 0.35 m step height.
    const glm::vec3 low = walkInto([](Scene& scene) { addBox(scene, {4.0f, 0.125f, 0.0f}, {2.5f, 0.125f, 3.0f}); });
    CHECK_MESSAGE((low.x > 3.0f && near(low.y, 0.25f + standingHeight, 0.1f)), "a character walks up a low step");

    // A 0.8 m ledge is a wall.
    const glm::vec3 high = walkInto([](Scene& scene) { addBox(scene, {4.0f, 0.4f, 0.0f}, {2.5f, 0.4f, 3.0f}); });
    CHECK_MESSAGE((high.x < 1.5f && near(high.y, standingHeight, 0.1f)), "a character cannot walk up a tall ledge");
}

TEST_CASE("CharacterController: slope limit") {
    // Ramps rising along +x from x = 2.
    const auto gentle = [](Scene& scene) {
        const float angle = glm::radians(20.0f);
        addBox(scene, {2.0f + 4.0f * std::cos(angle), 4.0f * std::sin(angle) - 0.1f, 0.0f}, {4.0f, 0.1f, 3.0f},
               PhysicsMotionType::Static, angle);
    };
    const auto steep = [](Scene& scene) {
        const float angle = glm::radians(50.0f);
        addBox(scene, {2.0f + 4.0f * std::cos(angle), 4.0f * std::sin(angle) - 0.1f, 0.0f}, {4.0f, 0.1f, 3.0f},
               PhysicsMotionType::Static, angle);
    };
    CharacterControllerComponent controller;
    controller.slopeLimitRadians = glm::radians(35.0f);
    const glm::vec3 climbed = walkInto(gentle, controller);
    CHECK_MESSAGE(climbed.y > standingHeight + 0.8f, "a character walks up a slope within its limit");
    const glm::vec3 refused = walkInto(steep, controller);
    CHECK_MESSAGE((refused.y < standingHeight + 0.4f && refused.x < 2.5f),
                  "a character cannot walk up a slope beyond its limit");
}

TEST_CASE("CharacterController: moving platform") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    // A platform whose top is at y = 0, with nothing else to stand on.
    const Entity platform = addBox(scene, {0.0f, -0.25f, 0.0f}, {2.0f, 0.25f, 2.0f}, PhysicsMotionType::Kinematic);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f});
    step(physics, scene, assets, 30);
    CHECK_MESSAGE(controllerOf(scene, character).grounded, "a character stands on a kinematic platform");

    const float dt = physics.config().fixedTimeStep;
    for (int index = 1; index <= 120; ++index) {
        scene.registry().get<TransformComponent>(platform).local =
            glm::translate(glm::mat4(1.0f), {static_cast<float>(index) * dt, -0.25f, 0.0f});
        scene.markTransformDirty(platform);
        step(physics, scene, assets);
    }
    const glm::vec3 position = positionOf(scene, character);
    const CharacterControllerComponent& controller = controllerOf(scene, character);
    CHECK_MESSAGE((near(position.x, 2.0f, 0.15f) && near(position.y, standingHeight, 0.1f)),
                  "a character rides a moving platform");
    CHECK_MESSAGE((controller.grounded && near(controller.groundVelocity.x, 1.0f, 0.1f)),
                  "a character on a moving platform reports the platform's velocity");
}

TEST_CASE("CharacterController: leaving a moving platform keeps its velocity") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    // A wide platform moving along +x at 1 m/s, with nothing else around.
    const Entity platform = addBox(scene, {0.0f, -0.25f, 0.0f}, {6.0f, 0.25f, 2.0f}, PhysicsMotionType::Kinematic);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f});
    step(physics, scene, assets, 30);
    const float dt = physics.config().fixedTimeStep;
    int frame = 0;
    const auto ride = [&](int steps) {
        for (int index = 0; index < steps; ++index) {
            ++frame;
            scene.registry().get<TransformComponent>(platform).local =
                glm::translate(glm::mat4(1.0f), {static_cast<float>(frame) * dt, -0.25f, 0.0f});
            scene.markTransformDirty(platform);
            step(physics, scene, assets);
        }
    };
    ride(60);
    CHECK_MESSAGE(controllerOf(scene, character).grounded, "the character rides the platform");

    controllerOf(scene, character).jumpRequested = true;
    ride(15);
    const CharacterControllerComponent& controller = controllerOf(scene, character);
    CHECK_MESSAGE((!controller.grounded && near(controller.velocity.x, 1.0f, 0.15f)),
                  "a character jumping off a moving platform keeps the platform's horizontal velocity");
}

TEST_CASE("CharacterController: jump") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f});
    step(physics, scene, assets, 30);
    const CharacterControllerComponent& controller = controllerOf(scene, character);
    CHECK_MESSAGE((controller.grounded && near(controller.groundNormal.y, 1.0f, 0.01f)),
                  "a character settles on the floor");
    CHECK_MESSAGE((controller.groundDistance >= 0.0f && controller.groundDistance < 0.05f),
                  "the ground probe finds the floor under a standing character");

    controllerOf(scene, character).jumpRequested = true;
    step(physics, scene, assets, 5);
    CHECK_MESSAGE((!controller.grounded && controller.velocity.y > 0.0f && !controller.jumpRequested),
                  "a jump request lifts the character off the ground and is consumed");
    float peak = 0.0f;
    for (int index = 0; index < 120; ++index) {
        step(physics, scene, assets);
        peak = std::max(peak, positionOf(scene, character).y);
    }
    // jumpSpeed 5.5 m/s rises about 1.5 m.
    CHECK_MESSAGE(peak > standingHeight + 1.2f, "a jump rises by its jump speed");
    CHECK_MESSAGE((controller.grounded && near(positionOf(scene, character).y, standingHeight, 0.1f)),
                  "a jumping character lands");

    controllerOf(scene, character).jumpRequested = true;
    step(physics, scene, assets);
    controllerOf(scene, character).jumpRequested = true;
    step(physics, scene, assets, 5);
    CHECK_MESSAGE(positionOf(scene, character).y < standingHeight + 0.6f, "an airborne character cannot jump again");
}

TEST_CASE("CharacterController: free motion") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const float start = standingHeight + 2.0f;
    const Entity character = addCharacter(scene, {0.0f, start, 0.0f});
    CharacterControllerComponent& controller = controllerOf(scene, character);
    controller.motion = CharacterMotion::Free;
    step(physics, scene, assets, 60);
    CHECK_MESSAGE(near(positionOf(scene, character).y, start, 0.02f), "a free character does not fall");

    controller.desiredVerticalSpeed = 1.0f;
    controller.jumpRequested = true;
    step(physics, scene, assets, 60);
    CHECK_MESSAGE((near(positionOf(scene, character).y, start + 1.0f, 0.15f) && !controller.jumpRequested),
                  "a free character rises at its vertical speed, and a jump request is consumed without a jump");

    controller.desiredVerticalSpeed = 0.0f;
    controller.desiredVelocity = {2.0f, 0.0f, 0.0f};
    step(physics, scene, assets, 60);
    const glm::vec3 swum = positionOf(scene, character);
    CHECK_MESSAGE((swum.x > 1.5f && near(swum.y, start + 1.0f, 0.15f)),
                  "a free character moves horizontally without sinking");

    controller.motion = CharacterMotion::Walking;
    controller.desiredVelocity = glm::vec3(0.0f);
    step(physics, scene, assets, 120);
    CHECK_MESSAGE((controller.grounded && near(positionOf(scene, character).y, standingHeight, 0.1f)),
                  "back to walking, the character falls to the floor");
}

TEST_CASE("CharacterController: ground probe and teleport") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.15f, 0.0f});
    step(physics, scene, assets);
    CHECK_MESSAGE(near(controllerOf(scene, character).groundDistance, 0.15f, 0.05f),
                  "the ground probe measures a short drop");

    // Gameplay moves the entity; the character follows it.
    scene.registry().get<TransformComponent>(character).local = glm::translate(glm::mat4(1.0f), {5.0f, 8.0f, 0.0f});
    scene.markTransformDirty(character);
    step(physics, scene, assets);
    const glm::vec3 teleported = positionOf(scene, character);
    CHECK_MESSAGE((near(teleported.x, 5.0f, 0.01f) && teleported.y > 7.5f),
                  "a character follows its entity when moved");
    CHECK_MESSAGE((!controllerOf(scene, character).grounded && controllerOf(scene, character).groundDistance < 0.0f),
                  "a character high above the floor finds no ground");

    // Interpolation blends the last step for rendering.
    step(physics, scene, assets);
    const float current = positionOf(scene, character).y;
    physics.interpolate(scene, 0.0f);
    CHECK_MESSAGE(positionOf(scene, character).y > current, "interpolation shows the previous step's position");
    physics.interpolate(scene, 1.0f);
    step(physics, scene, assets);
    CHECK_MESSAGE(positionOf(scene, character).y < current, "interpolated positions are not mistaken for a teleport");
}

TEST_CASE("CharacterController: queries and sensors") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f});
    const Entity sensor = scene.createEntity("Sensor");
    scene.registry().get<TransformComponent>(sensor).local = glm::translate(glm::mat4(1.0f), {3.0f, 1.0f, 0.0f});
    scene.registry().emplace<ColliderComponent>(sensor, BoxCollider{glm::vec3(0.5f)});
    scene.registry().emplace<RigidBodyComponent>(sensor, RigidBodyComponent{.sensor = true});
    step(physics, scene, assets, 10);

    PhysicsRayHit hit;
    CHECK_MESSAGE((physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) && hit.entity == character),
                  "rays hit a character as its entity");
    const Entity self[] = {character};
    CHECK_MESSAGE((physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit, {.ignore = self}) &&
                   hit.entity != character),
                  "a ray can ignore a character");

    bool entered = false;
    controllerOf(scene, character).desiredVelocity = {3.0f, 0.0f, 0.0f};
    for (int index = 0; index < 90 && !entered; ++index) {
        step(physics, scene, assets);
        for (const TriggerEvent& event : physics.triggerEvents()) {
            entered = entered || (event.type == TriggerEventType::Entered && event.sensor == sensor &&
                                  event.other == character && event.otherId == scene.id(character));
        }
    }
    CHECK_MESSAGE(entered, "a character walking into a sensor enters it");

    scene.registry().remove<CharacterControllerComponent>(character);
    step(physics, scene, assets);
    bool exited = false;
    for (const TriggerEvent& event : physics.triggerEvents()) {
        exited = exited || (event.type == TriggerEventType::Exited && event.other == character);
    }
    CHECK_MESSAGE(exited, "removing a character inside a sensor exits it");
    CHECK_MESSAGE((!physics.raycast({3.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) || hit.entity != character),
                  "a removed character leaves no inner body");
}

TEST_CASE("CharacterController: character replaces rigid body") {
    PhysicsWorld physics;
    CHECK_MESSAGE((static_cast<bool>(physics.init())), "physics initializes");
    Scene scene;
    AssetManager assets;
    addFloor(scene);
    const Entity character = addCharacter(scene, {0.0f, standingHeight + 0.05f, 0.0f});
    scene.registry().emplace<ColliderComponent>(character, CapsuleCollider{0.5f, 0.4f});
    scene.registry().emplace<RigidBodyComponent>(character,
                                                 RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
    scene.markPhysicsDirty(character);
    step(physics, scene, assets, 30);
    CHECK_MESSAGE(
        (physics.linearVelocity(character) == glm::vec3(0.0f) && !physics.setLinearVelocity(character, {1, 0, 0})),
        "a character controller replaces the entity's rigid body");
    CHECK_MESSAGE(controllerOf(scene, character).grounded, "the character still stands on the floor");
}

TEST_CASE("CharacterController: schema migration") {
    const std::shared_ptr<const ComponentTypeRegistry> types = builtInComponentTypes();
    AssetManager assets;
    Scene original;
    const Entity character = addCharacter(original, {0.0f, 1.0f, 0.0f});
    std::string text;
    CHECK_MESSAGE(SceneDocument::serialize(original, assets, {.componentTypes = types}, text).success,
                  "a scene with a character serializes");
    nlohmann::json document = nlohmann::json::parse(text);
    // The demo's version 1 controller, before the engine took the key over.
    for (nlohmann::json& entity : document.at("entities")) {
        if (entity.at("components").contains("character_controller")) {
            entity["components"]["character_controller"] = {
                {"radius", 0.3},     {"half_height", 0.6},   {"move_speed", 6.0},  {"sprint_multiplier", 2},
                {"jump_speed", 4.0}, {"acceleration", 20.0}, {"air_control", 0.5}, {"look_sensitivity", 0.01},
                {"yaw", 1.0},        {"pitch", 0.5}};
        }
    }
    Scene loaded;
    const SceneDocumentResult result =
        SceneDocument::deserialize(document.dump(), assets, {.componentTypes = types}, loaded);
    CHECK_MESSAGE(result.success, "a version 1 character controller loads");
    if (!result) {
        std::cerr << result.error << '\n';
        return;
    }
    const Entity migrated = loaded.findEntity(original.id(character));
    const auto* controller =
        migrated != entt::null ? loaded.registry().try_get<CharacterControllerComponent>(migrated) : nullptr;
    const CharacterControllerComponent defaults;
    CHECK_MESSAGE((controller != nullptr && controller->radius == 0.3f && controller->halfHeight == 0.6f &&
                   controller->moveSpeed == 6.0f && controller->airControl == 0.5f &&
                   controller->stepHeight == defaults.stepHeight && controller->layer == PhysicsLayer::Player &&
                   controller->moveAction == "Move"),
                  "a version 1 controller keeps its tuning and takes the new fields' defaults");

    std::string resaved;
    CHECK_MESSAGE((SceneDocument::serialize(loaded, assets, {.componentTypes = types}, resaved).success &&
                   resaved.find("look_sensitivity") == std::string::npos &&
                   resaved.find("\"$version\": 2") != std::string::npos),
                  "a migrated controller saves as version 2 without the look settings");
}

// The engine's first-person systems: input walks a character relative to its
// camera's yaw, and look input turns the camera within its pitch limits.
TEST_CASE("CharacterController: first person input") {
    Scene scene;
    const Entity player = spawnFirstPersonPlayer(scene, {0.0f, 1.0f, 0.0f});
    scene.updateWorldTransforms();
    const Entity camera = findCameraChild(scene, player);
    CHECK_MESSAGE(
        (camera != entt::null && scene.registry().all_of<FirstPersonCameraComponent, AudioListenerComponent>(camera)),
        "a spawned player has a first-person camera that listens");
    if (camera == entt::null) {
        return;
    }
    CHECK_MESSAGE(near(scene.registry().get<TransformComponent>(camera).local[3].y, 0.9f, 1.0e-4f),
                  "the spawned camera sits at the top of the capsule");

    InputActionMap actions;
    actions.addAction("Move", InputActionValueType::Axis2D);
    actions.addBinding("Move", {.source = Key::W, .scale = {0.0f, 1.0f}});
    actions.addAction("Jump", InputActionValueType::Button);
    actions.addBinding("Jump", {.source = Key::Space});
    actions.addAction("Sprint", InputActionValueType::Button);
    actions.addBinding("Sprint", {.source = Key::LeftShift});
    // Mouse deltas are distances, not unit-length axes.
    actions.addAction("Look", InputActionValueType::Axis2D, false);
    actions.addBinding("Look", {.source = MouseAxis::DeltaX, .scale = {1.0f, 0.0f}});
    actions.addBinding("Look", {.source = MouseAxis::DeltaY, .scale = {0.0f, 1.0f}});

    InputState forward;
    forward.setKeyDown(Key::W);
    forward.setKeyPressed(Key::Space);
    applyCharacterInput(scene, actions.evaluate(forward));
    const CharacterControllerComponent& controller = scene.registry().get<CharacterControllerComponent>(player);
    CHECK_MESSAGE((near(controller.desiredVelocity.z, -controller.moveSpeed, 1.0e-4f) &&
                   near(controller.desiredVelocity.x, 0.0f, 1.0e-4f) && controller.jumpRequested),
                  "forward input walks a character toward -Z and requests a jump");

    // Turning the view a quarter left walks the character toward -X.
    scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians = glm::radians(90.0f);
    InputState sprint;
    sprint.setKeyDown(Key::W);
    sprint.setKeyDown(Key::LeftShift);
    applyCharacterInput(scene, actions.evaluate(sprint));
    CHECK_MESSAGE((near(controller.desiredVelocity.x, -controller.moveSpeed * controller.sprintMultiplier, 1.0e-3f) &&
                   near(controller.desiredVelocity.z, 0.0f, 1.0e-3f)),
                  "a character walks relative to its camera's yaw and sprints");

    scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians = 0.0f;
    InputState look;
    look.mouseDelta = {100.0f, -10000.0f};
    updateFirstPersonCameras(scene, nullptr, 0.016f);
    const InputActionState lookActions = actions.evaluate(look);
    updateFirstPersonCameras(scene, &lookActions, 0.016f);
    const auto& view = scene.registry().get<FirstPersonCameraComponent>(camera);
    CHECK_MESSAGE((view.yawRadians < 0.0f && view.pitchRadians == view.maximumPitchRadians),
                  "look input turns the camera and its pitch is clamped");
    CHECK_MESSAGE(near(scene.registry().get<TransformComponent>(camera).local[3].y, 0.9f, 1.0e-4f),
                  "turning the camera keeps its eye height");

    scene.registry().ctx().insert_or_assign(FirstPersonViewPreferences{.lookScale = 2.0f});
    const float yawBefore = view.yawRadians;
    updateFirstPersonCameras(scene, &lookActions, 0.016f);
    CHECK_MESSAGE(near(yawBefore - view.yawRadians, 2.0f * 100.0f * view.sensitivity, 1.0e-4f),
                  "the player's look scale multiplies look input");

    scene.registry().get<CharacterControllerComponent>(player).moveAction.clear();
    scene.registry().get<CharacterControllerComponent>(player).desiredVelocity = {1.0f, 0.0f, 0.0f};
    applyCharacterInput(scene, actions.evaluate(forward));
    CHECK_MESSAGE(controller.desiredVelocity.x == 1.0f, "a character without a move action is left to code");
}
} // namespace

TEST_SUITE_END();
