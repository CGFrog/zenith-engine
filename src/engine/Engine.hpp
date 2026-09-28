#pragma once

#include "animation/AnimationSystem.hpp"
#include "application/Application.hpp"
#include "assets/Assets.hpp"
#include "audio/AudioSystem.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"
#include "physics/PhysicsWorld.hpp"
#include "scene/Scene.hpp"
#include "ui/GameUi.hpp"
#include "window/Window.hpp"

class Engine {
  public:
    bool init(const PhysicsWorldConfig& physicsConfig = {}, const AudioConfig& audioConfig = {});
    bool run(Application& application);
    void shutdown();

    Scene& scene();
    AssetManager& assets();
    PhysicsWorld& physics();
    AudioSystem& audio();
    RenderRuntime& rendering();

  private:
    static uint64_t readFrameLimit();
    static uint64_t readBenchmarkFrameCount();

    Window m_window;
    InputManager m_input;
    Renderer m_renderer;
    RenderRuntime m_rendering;
    AssetManager m_assets;
    Scene m_scene;
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    GameUiContext m_gameUi;
    float m_physicsAccumulator{0.0f};
};
