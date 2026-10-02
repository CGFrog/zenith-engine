#pragma once

#include "animation/AnimationSystem.hpp"
#include "application/Application.hpp"
#include "assets/Assets.hpp"
#include "audio/AudioSystem.hpp"
#include "diagnostics/FrameCaptureConfig.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"
#include "jobs/FrameTaskGraph.hpp"
#include "physics/PhysicsWorld.hpp"
#include "scene/Scene.hpp"
#include "ui/GameUi.hpp"
#include "window/Window.hpp"

class Engine {
  public:
    Engine();
    bool init(const PhysicsWorldConfig& physicsConfig = {}, const AudioConfig& audioConfig = {});
    bool run(Application& application);
    void shutdown();

    Scene& scene();
    AssetManager& assets();
    PhysicsWorld& physics();
    AudioSystem& audio();
    RenderRuntime& rendering();
    // Validation-layer results, including messages emitted during shutdown.
    GraphicsValidationReport graphicsValidation() const;

  private:
    static uint64_t readFrameLimit();
    // Takes the requested capture from the frame just drawn and writes it.
    bool writeFrameCapture(const FrameCaptureRequest& request);

    FrameCaptureConfig m_captureConfig;
    Window m_window;
    InputManager m_input;
    FrameTaskExecutor m_frameTasks;
    Renderer m_renderer;
    RenderRuntime m_rendering;
    AssetManager m_assets;
    Scene m_scene;
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    GameUiContext m_gameUi;
    float m_physicsAccumulator{0.0f};
    GraphicsValidationOptions m_graphicsValidation;
};
