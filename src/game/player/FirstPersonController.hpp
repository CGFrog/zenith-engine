#pragma once

#include "engine/input/InputManager.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Scene.hpp"

struct CharacterControllerComponent {
    float radius{0.4f};
    float halfHeight{0.5f};
    float moveSpeed{4.0f};
    float sprintMultiplier{1.75f};
    float jumpSpeed{5.5f};
    float acceleration{30.0f};
    float airControl{0.25f};
    float lookSensitivity{0.0025f};
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    bool grounded{false};
    Entity camera{entt::null};
};

class FirstPersonControllerSystem {
  public:
    void updateView(Scene& scene, const InputState& input) const;
    void fixedUpdate(Scene& scene, PhysicsWorld& physics, const InputState& input, float deltaSeconds) const;
};
