#pragma once

#include "engine/input/InputManager.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Scene.hpp"

// A first-person player: a capsule body the movement system drives and a view
// the look system orients. Lengths are metres, speeds metres per second, and
// angles radians. Registered as the "character_controller" component type by
// registerDemoGameplay; grounded and camera are runtime state and are not
// serialized.
struct CharacterControllerComponent {
    float radius{0.4f};
    float halfHeight{0.5f};
    float moveSpeed{4.0f};
    float sprintMultiplier{1.75f};
    float jumpSpeed{5.5f};
    // Approach rate toward the desired horizontal velocity, per second.
    float acceleration{30.0f};
    // Fraction of acceleration available while airborne.
    float airControl{0.25f};
    // Radians of view rotation per unit of look input.
    float lookSensitivity{0.0025f};
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    bool grounded{false};
    // The entity whose transform shows the view. When null or destroyed, the
    // first child with a CameraComponent is adopted.
    Entity camera{entt::null};
};

// Player preferences that scale look input for every controller in a scene.
// The game stores them in the scene's registry context; when absent the
// defaults apply, as in an editor's play mode.
struct FirstPersonViewPreferences {
    float lookScale{1.0f};
    bool invertLookY{false};
};

struct FirstPersonControllerInput {
    glm::vec2 move{0.0f};
    glm::vec2 look{0.0f};
    bool jumpPressed{false};
    bool sprint{false};
    bool gamepadLook{false};
};

class FirstPersonControllerSystem {
  public:
    void updateView(Scene& scene, const FirstPersonControllerInput& input, float deltaSeconds) const;
    void fixedUpdate(Scene& scene, PhysicsWorld& physics, const FirstPersonControllerInput& input,
                     float deltaSeconds) const;
};
