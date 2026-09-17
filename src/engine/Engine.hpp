#pragma once

#include "assets/Assets.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"
#include "physics/PhysicsWorld.hpp"
#include "scene/FirstPersonController.hpp"
#include "scene/Scene.hpp"
#include "window/Window.hpp"

class Engine {
  public:
    bool init(const PhysicsWorldConfig& physicsConfig = {});
    bool run();
    void shutdown();

    Scene& scene();
    AssetManager& assets();
    PhysicsWorld& physics();

  private:
    static uint64_t readFrameLimit();

    Window m_window;
    InputManager m_input;
    Renderer m_renderer;
    AssetManager m_assets;
    Scene m_scene;
    FirstPersonControllerSystem m_firstPersonControllers;
    PhysicsWorld m_physics;
    float m_physicsAccumulator{0.0f};
    bool m_jumpQueued{false};
};
