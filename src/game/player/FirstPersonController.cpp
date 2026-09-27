#include "FirstPersonController.hpp"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

void FirstPersonControllerSystem::updateView(Scene& scene, const InputState& input) const {
    auto controllers = scene.registry().view<CharacterControllerComponent>();
    for (Entity entity : controllers) {
        auto& controller = controllers.get<CharacterControllerComponent>(entity);
        controller.yawRadians -= input.mouseDelta.x * controller.lookSensitivity;
        controller.pitchRadians = std::clamp(controller.pitchRadians - input.mouseDelta.y * controller.lookSensitivity,
                                             glm::radians(-89.0f), glm::radians(89.0f));

        if (controller.camera == entt::null || !scene.registry().valid(controller.camera)) {
            continue;
        }
        const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), controller.yawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 orientation =
            yaw * glm::rotate(glm::mat4(1.0f), controller.pitchRadians, glm::vec3(1.0f, 0.0f, 0.0f));
        scene.registry().get<TransformComponent>(controller.camera).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f)) *
            orientation;
    }
}

void FirstPersonControllerSystem::fixedUpdate(Scene& scene, PhysicsWorld& physics, const InputState& input,
                                              float deltaSeconds) const {
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
        const float inputX = static_cast<float>(input.keyDown(Key::D)) - static_cast<float>(input.keyDown(Key::A));
        const float inputZ = static_cast<float>(input.keyDown(Key::W)) - static_cast<float>(input.keyDown(Key::S));
        glm::vec3 direction = glm::vec3(yaw * glm::vec4(inputX, 0.0f, -inputZ, 0.0f));
        if (glm::dot(direction, direction) > 1.0f) {
            direction = glm::normalize(direction);
        }
        const bool sprint = input.keyDown(Key::LeftShift) || input.keyDown(Key::RightShift);
        const float speed = controller.moveSpeed * (sprint ? controller.sprintMultiplier : 1.0f);
        const glm::vec2 desired(direction.x * speed, direction.z * speed);
        glm::vec2 horizontal(velocity.x, velocity.z);
        const float control = controller.grounded ? 1.0f : controller.airControl;
        const float blend = std::min(1.0f, controller.acceleration * control * std::max(deltaSeconds, 0.0f));
        horizontal = glm::mix(horizontal, desired, blend);
        velocity.x = horizontal.x;
        velocity.z = horizontal.y;
        if (input.keyPressed(Key::Space) && controller.grounded) {
            velocity.y = controller.jumpSpeed;
        }
        physics.setLinearVelocity(entity, velocity);
    }
}
