#pragma once

#include "animation/AnimationSystem.hpp"
#include "application/Application.hpp"
#include "assets/Assets.hpp"
#include "audio/AudioSystem.hpp"
#include "diagnostics/FrameCaptureConfig.hpp"
#include "diagnostics/FrameTimings.hpp"
#include "diagnostics/Logger.hpp"
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
    // Engine and subsystem messages; outlives every subsystem the engine owns.
    Logger& logger();
    // Validation-layer results, including messages emitted during shutdown.
    GraphicsValidationReport graphicsValidation() const;

  private:
    static uint64_t readFrameLimit();
    // Takes the requested capture from the frame just drawn and writes it.
    bool writeFrameCapture(const FrameCaptureRequest& request);
    // A capture run renders every frame at one drawable size; records the
    // first frame's size in firstSize and reports a later change.
    bool captureWindowSizeUnchanged(std::optional<WindowSizeOverride>& firstSize);

    // Declared first: subsystems log through it until they are destroyed.
    Logger m_logger;
    LogChannel m_log{&m_logger, "engine"};
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
    // The last completed frame's CPU phase timings, shared with the
    // application through ApplicationContext::frameTimings.
    EngineFrameTimings m_frameTimings;
    GraphicsValidationOptions m_graphicsValidation;
};
