#include "FirstPersonController.hpp"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace {
// The controller's camera, adopting the first camera child when none is
// assigned, as for a controller loaded from a scene document.
Entity resolveCamera(Scene& scene, Entity entity, CharacterControllerComponent& controller) {
    entt::registry& registry = scene.registry();
    if (controller.camera != entt::null && registry.valid(controller.camera) &&
        registry.all_of<TransformComponent>(controller.camera)) {
        return controller.camera;
    }
    controller.camera = entt::null;
    if (const auto* relationship = registry.try_get<RelationshipComponent>(entity)) {
        for (Entity child : relationship->children) {
            if (registry.valid(child) && registry.all_of<CameraComponent, TransformComponent>(child)) {
                controller.camera = child;
                break;
            }
        }
    }
    return controller.camera;
}
} // namespace

void FirstPersonControllerSystem::updateView(Scene& scene, const FirstPersonControllerInput& input,
                                             float deltaSeconds) const {
    auto controllers = scene.registry().view<CharacterControllerComponent>();
    for (Entity entity : controllers) {
        auto& controller = controllers.get<CharacterControllerComponent>(entity);
        const float lookScale = input.gamepadLook ? 800.0f * std::max(deltaSeconds, 0.0f) : 1.0f;
        controller.yawRadians -= input.look.x * controller.lookSensitivity * lookScale;
        controller.pitchRadians =
            std::clamp(controller.pitchRadians - input.look.y * controller.lookSensitivity * lookScale,
                       glm::radians(-89.0f), glm::radians(89.0f));

        if (resolveCamera(scene, entity, controller) == entt::null) {
            continue;
        }
        const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), controller.yawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 orientation =
            yaw * glm::rotate(glm::mat4(1.0f), controller.pitchRadians, glm::vec3(1.0f, 0.0f, 0.0f));
        scene.registry().get<TransformComponent>(controller.camera).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f)) *
            orientation;
        scene.markTransformDirty(controller.camera);
    }
}

void FirstPersonControllerSystem::fixedUpdate(Scene& scene, PhysicsWorld& physics,
                                              const FirstPersonControllerInput& input, float deltaSeconds) const {
    auto controllers = scene.registry().view<CharacterControllerComponent, RigidBodyComponent>();
    for (Entity entity : controllers) {
        auto& controller = controllers.get<CharacterControllerComponent>(entity);
        controller.grounded = false;
        for (const ContactEvent& contact : physics.contactEvents()) {
            if (contact.first == entity && contact.normal.y < -0.45f) {
                controller.grounded = true;
            } else if (contact.second == entity && contact.normal.y > 0.45f) {
                controller.grounded = true;
            }
        }

        glm::vec3 velocity = physics.linearVelocity(entity);
        const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), controller.yawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 direction = glm::vec3(yaw * glm::vec4(input.move.x, 0.0f, -input.move.y, 0.0f));
        if (glm::dot(direction, direction) > 1.0f) {
            direction = glm::normalize(direction);
        }
        const float speed = controller.moveSpeed * (input.sprint ? controller.sprintMultiplier : 1.0f);
        const glm::vec2 desired(direction.x * speed, direction.z * speed);
        glm::vec2 horizontal(velocity.x, velocity.z);
        const float control = controller.grounded ? 1.0f : controller.airControl;
        const float blend = std::min(1.0f, controller.acceleration * control * std::max(deltaSeconds, 0.0f));
        horizontal = glm::mix(horizontal, desired, blend);
        velocity.x = horizontal.x;
        velocity.z = horizontal.y;
        if (input.jumpPressed && controller.grounded) {
            velocity.y = controller.jumpSpeed;
        }
        physics.setLinearVelocity(entity, velocity);
    }
}
