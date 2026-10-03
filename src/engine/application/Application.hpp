#pragma once

#include "../assets/Assets.hpp"
#include "../audio/AudioSystem.hpp"
#include "../diagnostics/FrameTimings.hpp"
#include "../diagnostics/Logger.hpp"
#include "../graphics/Rendering.hpp"
#include "../input/InputManager.hpp"
#include "../physics/PhysicsWorld.hpp"
#include "../scene/Scene.hpp"
#include "../ui/GameUi.hpp"
#include "SimulationClock.hpp"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct SceneKey {
    uint32_t value{0};

    auto operator<=>(const SceneKey&) const = default;
};

class ApplicationControl {
  public:
    void requestSceneChange(SceneKey scene);
    void requestQuit();

    bool actionPending() const;

  private:
    friend class ApplicationSession;

    void clear();

    std::optional<SceneKey> m_requestedScene;
    bool m_quitRequested{false};
};

struct ApplicationContext {
    Scene& scene;
    AssetManager& assets;
    PhysicsWorld& physics;
    AudioSystem& audio;
    ApplicationControl& control;
    RenderRuntime& rendering;
    GameUiContext& gameUi;
    // The engine's log; scenes and tools may log through it and read it.
    Logger& logger;
    // The engine loop's CPU phase timings for the last completed frame; null
    // when the host does not measure them (for example in tests).
    const EngineFrameTimings* frameTimings{nullptr};
};

class ApplicationScene {
  public:
    virtual ~ApplicationScene() = default;

    virtual SceneResult load(ApplicationContext& context) = 0;
    virtual void update(ApplicationContext& context, const InputState& input, float deltaSeconds) = 0;
    virtual void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) = 0;
    virtual bool fixedUpdatesEnabled() const;
    virtual bool beginFixedUpdate();
    // Read each frame after update (so a change made there applies to that
    // frame's fixed steps, animation, and audio); the default runs at
    // wall-clock rate.
    virtual SimulationClock simulationClock() const;
    virtual void requestQuit(ApplicationContext& context);
    virtual void drawUi(ApplicationContext& context);
    virtual void unload(ApplicationContext& context) = 0;
};

class Application {
  public:
    virtual ~Application() = default;

    virtual SceneResult onStart(ApplicationContext& context);
    virtual MouseCapturePolicy mouseCapturePolicy() const;
    virtual SceneKey initialScene() const = 0;
    virtual std::unique_ptr<ApplicationScene> createScene(SceneKey scene) = 0;
    virtual void onStop(ApplicationContext& context);
};

struct ApplicationFrameResult {
    bool success{false};
    bool keepRunning{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Owns application lifecycle state independently of the window and renderer so
// transitions can be tested without a GPU.
class ApplicationSession {
  public:
    ApplicationSession(Application& application, Scene& scene, AssetManager& assets, PhysicsWorld& physics,
                       AudioSystem& audio);
    ApplicationSession(Application& application, Scene& scene, AssetManager& assets, PhysicsWorld& physics,
                       AudioSystem& audio, RenderRuntime& rendering);
    ApplicationSession(Application& application, Scene& scene, AssetManager& assets, PhysicsWorld& physics,
                       AudioSystem& audio, RenderRuntime& rendering, GameUiContext& gameUi, Logger& logger);
    ~ApplicationSession();

    ApplicationSession(const ApplicationSession&) = delete;
    ApplicationSession& operator=(const ApplicationSession&) = delete;

    SceneResult start();
    void update(const InputState& input, float deltaSeconds);
    void fixedUpdate(const InputState& input, float deltaSeconds);
    bool fixedUpdatesEnabled() const;
    bool beginFixedUpdate();
    // The active scene's clock, or the default one without a scene.
    SimulationClock simulationClock() const;
    // Shares the host's frame timings with the scenes (see
    // ApplicationContext::frameTimings); timings must outlive the session.
    void setFrameTimings(const EngineFrameTimings* timings);
    void drawUi();
    ApplicationFrameResult finishFrame();
    bool actionPending() const;
    void requestQuit();
    void shutdown();

  private:
    SceneResult activate(SceneKey scene);
    SceneResult transition(SceneKey scene);
    void deactivate();

    Application& m_application;
    ApplicationControl m_control;
    RenderRuntime m_fallbackRendering;
    GameUiContext m_fallbackGameUi;
    Logger m_fallbackLogger;
    ApplicationContext m_context;
    std::unique_ptr<ApplicationScene> m_activeScene;
    AssetLeaseSet m_sceneAssets;
    bool m_started{false};
};
