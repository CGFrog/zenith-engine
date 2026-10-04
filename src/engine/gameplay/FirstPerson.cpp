#include "FirstPerson.hpp"

#include "engine/audio/AudioSystem.hpp"
#include "engine/input/InputActions.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

// The first camera child that has a first-person view, or null.
Entity firstPersonCamera(const Scene& scene, Entity character) {
    const entt::registry& registry = scene.registry();
    if (const auto* relationship = registry.try_get<RelationshipComponent>(character)) {
        for (const Entity child : relationship->children) {
            if (registry.valid(child) && registry.all_of<FirstPersonCameraComponent, TransformComponent>(child)) {
                return child;
            }
        }
    }
    return entt::null;
}

// The rotation about world Y that turns -Z toward the matrix's forward axis.
float yawOf(const glm::mat4& world) {
    const glm::vec3 forward = -glm::vec3(world[2]);
    if (std::abs(forward.x) + std::abs(forward.z) <= 1.0e-6f) {
        return 0.0f;
    }
    return std::atan2(-forward.x, -forward.z);
}
} // namespace

Entity findCameraChild(const Scene& scene, Entity entity) {
    const entt::registry& registry = scene.registry();
    if (const auto* relationship = registry.try_get<RelationshipComponent>(entity)) {
        for (const Entity child : relationship->children) {
            if (registry.valid(child) && registry.all_of<CameraComponent>(child)) {
                return child;
            }
        }
    }
    return entt::null;
}

float characterHeading(const Scene& scene, Entity character) {
    const entt::registry& registry = scene.registry();
    const float ownYaw = yawOf(registry.get<TransformComponent>(character).world);
    const Entity camera = firstPersonCamera(scene, character);
    return camera == entt::null ? ownYaw : ownYaw + registry.get<FirstPersonCameraComponent>(camera).yawRadians;
}

void applyCharacterInput(Scene& scene, const InputActionState& actions) {
    entt::registry& registry = scene.registry();
    for (const Entity entity : registry.view<CharacterControllerComponent, TransformComponent>()) {
        auto& controller = registry.get<CharacterControllerComponent>(entity);
        if (controller.moveAction.empty()) {
            continue;
        }
        const glm::vec2 move = actions.axis2D(controller.moveAction);
        const glm::mat4 heading = glm::rotate(glm::mat4(1.0f), characterHeading(scene, entity), worldUp);
        glm::vec3 direction = glm::vec3(heading * glm::vec4(move.x, 0.0f, -move.y, 0.0f));
        if (glm::dot(direction, direction) > 1.0f) {
            direction = glm::normalize(direction);
        }
        const bool sprinting = !controller.sprintAction.empty() && actions.down(controller.sprintAction);
        controller.desiredVelocity =
            direction * controller.moveSpeed * (sprinting ? controller.sprintMultiplier : 1.0f);
        if (!controller.jumpAction.empty() && actions.pressed(controller.jumpAction)) {
            controller.jumpRequested = true;
        }
    }
}

void updateFirstPersonCameras(Scene& scene, const InputActionState* actions, float deltaSeconds) {
    entt::registry& registry = scene.registry();
    const auto* stored = registry.ctx().find<FirstPersonViewPreferences>();
    const FirstPersonViewPreferences preferences = stored ? *stored : FirstPersonViewPreferences{};
    for (const Entity entity : registry.view<FirstPersonCameraComponent, TransformComponent>()) {
        auto& view = registry.get<FirstPersonCameraComponent>(entity);
        if (actions != nullptr && !view.lookAction.empty()) {
            glm::vec2 look = actions->axis2D(view.lookAction) * preferences.lookScale;
            if (preferences.invertLookY) {
                look.y = -look.y;
            }
            // A stick reports a rate; a mouse reports a distance per frame.
            if (actions->activeDevice() == InputDevice::Gamepad) {
                look *= view.gamepadLookSpeed * std::max(deltaSeconds, 0.0f);
            }
            view.yawRadians -= look.x * view.sensitivity;
            view.pitchRadians -= look.y * view.sensitivity;
        }
        view.pitchRadians = std::clamp(view.pitchRadians, view.minimumPitchRadians,
                                       std::max(view.minimumPitchRadians, view.maximumPitchRadians));

        auto& transform = registry.get<TransformComponent>(entity);
        const glm::vec3 eye(transform.local[3]);
        transform.local = glm::translate(glm::mat4(1.0f), eye) *
                          glm::rotate(glm::mat4(1.0f), view.yawRadians, worldUp) *
                          glm::rotate(glm::mat4(1.0f), view.pitchRadians, glm::vec3(1.0f, 0.0f, 0.0f));
        scene.markTransformDirty(entity);
    }
}

Entity spawnFirstPersonPlayer(Scene& scene, const glm::vec3& position, const CharacterControllerComponent& controller) {
    entt::registry& registry = scene.registry();
    const Entity player = scene.createEntity("Player");
    registry.get<TransformComponent>(player).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(player);
    registry.emplace<CharacterControllerComponent>(player, controller);

    const Entity camera = scene.createEntity("Player Camera");
    registry.emplace<CameraComponent>(camera);
    registry.emplace<FirstPersonCameraComponent>(camera);
    registry.emplace<AudioListenerComponent>(camera);
    scene.setParent(camera, player);
    // The eye sits at the top of the capsule.
    registry.get<TransformComponent>(camera).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f));
    scene.markTransformDirty(camera);
    return player;
}
