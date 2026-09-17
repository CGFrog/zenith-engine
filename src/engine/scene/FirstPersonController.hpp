#pragma once

#include "../input/InputManager.hpp"
#include "../physics/PhysicsWorld.hpp"
#include "Scene.hpp"

class FirstPersonControllerSystem {
  public:
    void updateView(Scene& scene, const InputState& input) const;
    void fixedUpdate(Scene& scene, PhysicsWorld& physics, const InputState& input, float deltaSeconds) const;
};
