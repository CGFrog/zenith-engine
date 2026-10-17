// The application session as games run it: scene start, deferred switches,
// failed loads and transitions, shutdown order, mouse input, and the demo
// application playing, turning its camera, and reloading.

#include "engine/application/Application.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "game/application/DemoApplication.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("ApplicationLifecycle");

namespace {

struct RecordingState {
    std::vector<std::string> events;
};

class RecordingScene final : public ApplicationScene {
  public:
    RecordingScene(RecordingState& state, uint32_t id)
        : m_state(state)
        , m_id(id) {}

    SceneResult load(ApplicationContext& context) override {
        m_state.events.push_back("load" + std::to_string(m_id));
        context.scene.createEntity("Recording Scene " + std::to_string(m_id));
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState& input, float) override {
        m_state.events.push_back("update" + std::to_string(m_id));
        if (m_id == 0 && input.keyPressed(Key::R)) {
            context.control.requestSceneChange({1});
        }
        if (m_id == 1 && input.keyPressed(Key::Q)) {
            context.control.requestQuit();
        }
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {
        m_state.events.push_back("fixed" + std::to_string(m_id));
    }

    void drawUi(ApplicationContext&) override {
        m_state.events.push_back("ui" + std::to_string(m_id));
    }

    void unload(ApplicationContext&) override {
        m_state.events.push_back("unload" + std::to_string(m_id));
    }

  private:
    RecordingState& m_state;
    uint32_t m_id;
};

class RecordingApplication final : public Application {
  public:
    explicit RecordingApplication(RecordingState& state)
        : m_state(state) {}

    SceneResult onStart(ApplicationContext&) override {
        m_state.events.push_back("start");
        return {.success = true};
    }

    SceneKey initialScene() const override {
        return {0};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override {
        if (scene.value > 1) {
            return nullptr;
        }
        return std::make_unique<RecordingScene>(m_state, scene.value);
    }

    void onStop(ApplicationContext&) override {
        m_state.events.push_back("stop");
    }

  private:
    RecordingState& m_state;
};

class FailingScene final : public ApplicationScene {
  public:
    explicit FailingScene(RecordingState& state)
        : m_state(state) {}

    SceneResult load(ApplicationContext& context) override {
        m_state.events.push_back("failing-load");
        context.scene.createEntity("Partial State");
        return {.error = "expected load failure"};
    }

    void update(ApplicationContext&, const InputState&, float) override {}

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {
        m_state.events.push_back("failing-unload");
    }

  private:
    RecordingState& m_state;
};

class FailingApplication final : public Application {
  public:
    explicit FailingApplication(RecordingState& state)
        : m_state(state) {}

    SceneKey initialScene() const override {
        return {0};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<FailingScene>(m_state);
    }

    void onStop(ApplicationContext&) override {
        m_state.events.push_back("failing-stop");
    }

  private:
    RecordingState& m_state;
};

class FailingTransitionScene final : public ApplicationScene {
  public:
    explicit FailingTransitionScene(bool shouldFail)
        : m_shouldFail(shouldFail) {}

    SceneResult load(ApplicationContext& context) override {
        context.scene.createEntity(m_shouldFail ? "Partial replacement" : "Preserved scene");
        return m_shouldFail ? SceneResult{.error = "replacement failed"} : SceneResult{.success = true};
    }

    void update(ApplicationContext& context, const InputState& input, float) override {
        if (!m_shouldFail && input.keyPressed(Key::R)) {
            context.control.requestSceneChange({1});
        }
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    bool m_shouldFail{false};
};

class FailingTransitionApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {0};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override {
        return std::make_unique<FailingTransitionScene>(scene.value == 1);
    }
};

// A scene that frees the pointer while its menu is open, as a game's does.
class PointerScene final : public ApplicationScene {
  public:
    explicit PointerScene(const bool& menuOpen)
        : m_menuOpen(menuOpen) {}

    SceneResult load(ApplicationContext&) override {
        return {.success = true};
    }

    void update(ApplicationContext&, const InputState&, float) override {}

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

    MouseCapturePolicy mouseCapturePolicy(MouseCapturePolicy application) const override {
        return m_menuOpen ? MouseCapturePolicy::Released : application;
    }

  private:
    const bool& m_menuOpen;
};

class PointerApplication final : public Application {
  public:
    explicit PointerApplication(const bool& menuOpen)
        : m_menuOpen(menuOpen) {}

    MouseCapturePolicy mouseCapturePolicy() const override {
        return MouseCapturePolicy::WhileRightMouseDown;
    }

    SceneKey initialScene() const override {
        return {0};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<PointerScene>(m_menuOpen);
    }

  private:
    const bool& m_menuOpen;
};

size_t entityCount(const Scene& scene) {
    const auto view = scene.registry().view<TransformComponent>();
    return static_cast<size_t>(std::distance(view.begin(), view.end()));
}

TEST_CASE("ApplicationLifecycle: mouse buttons and wheel reach gameplay") {
    InputState input;
    input.setMouseButtonDown(MouseButton::Left);
    input.setMouseButtonPressed(MouseButton::Right);
    input.mouseWheelDelta = {1.0f, -2.0f};

    CHECK_MESSAGE(input.mouseButtonDown(MouseButton::Left), "mouse held state is exposed to gameplay");
    CHECK_MESSAGE(!input.mouseButtonDown(MouseButton::Right), "mouse held state remains button-specific");
    CHECK_MESSAGE(input.mouseButtonPressed(MouseButton::Right), "mouse press edges are exposed to gameplay");
    CHECK_MESSAGE(input.mouseWheelDelta == glm::vec2(1.0f, -2.0f), "mouse wheel motion preserves both axes");
}

TEST_CASE("ApplicationLifecycle: a session starts switches and shuts down scenes in order") {
    RecordingState state;
    RecordingApplication application(state);
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);

    CHECK_MESSAGE((static_cast<bool>(session.start())), "application session starts and loads its initial scene");
    CHECK_MESSAGE((state.events == std::vector<std::string>({"start", "load0"})),
                  "startup callbacks run in deterministic order");
    CHECK_MESSAGE(entityCount(scene) == 1, "initial scene owns its ECS state");

    session.fixedUpdate({}, 1.0f / 60.0f);
    session.drawUi();
    InputState reload;
    reload.setKeyPressed(Key::R);
    session.update(reload, 1.0f / 60.0f);
    CHECK_MESSAGE(session.actionPending(), "scene changes remain pending during callbacks");
    CHECK_MESSAGE(entityCount(scene) == 1, "pending transition does not mutate the active scene");
    const ApplicationFrameResult switched = session.finishFrame();
    CHECK_MESSAGE((switched && switched.keepRunning), "deferred scene transition succeeds");
    CHECK_MESSAGE(entityCount(scene) == 1, "old scene state is cleared before the new scene loads");
    CHECK_MESSAGE(
        (state.events == std::vector<std::string>({"start", "load0", "fixed0", "ui0", "update0", "load1", "unload0"})),
        "switch stages the new scene before unloading the old scene");

    InputState quit;
    quit.setKeyPressed(Key::Q);
    session.update(quit, 1.0f / 60.0f);
    const ApplicationFrameResult stopped = session.finishFrame();
    CHECK_MESSAGE((stopped && !stopped.keepRunning), "application quit request ends the session loop");
    session.shutdown();
    CHECK_MESSAGE((state.events[state.events.size() - 2] == "unload1" && state.events.back() == "stop"),
                  "shutdown unloads the active scene before stopping the application");
    CHECK_MESSAGE(entityCount(scene) == 0, "shutdown clears all ECS state");
}

TEST_CASE("ApplicationLifecycle: the active scene adjusts the mouse capture policy") {
    bool menuOpen = false;
    PointerApplication application(menuOpen);
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    CHECK_MESSAGE(session.mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "without an active scene the application's policy applies");
    CHECK_MESSAGE((static_cast<bool>(session.start())), "the pointer test starts its scene");
    CHECK_MESSAGE(session.mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "a scene keeps the application's policy by default");
    menuOpen = true;
    CHECK_MESSAGE(session.mouseCapturePolicy() == MouseCapturePolicy::Released, "a scene's menu frees the pointer");
    session.shutdown();
}

TEST_CASE("ApplicationLifecycle: a failed transition keeps the active scene") {
    FailingTransitionApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    CHECK_MESSAGE((static_cast<bool>(session.start())), "transition test starts its initial scene");

    const SceneEntityId preserved = scene.id(*scene.registry().view<NameComponent>().begin());
    InputState reload;
    reload.setKeyPressed(Key::R);
    session.update(reload, 1.0f / 60.0f);
    const ApplicationFrameResult result = session.finishFrame();
    CHECK_MESSAGE((!result && result.error == "replacement failed"), "failed transitions return their load error");
    CHECK_MESSAGE((entityCount(scene) == 1 && scene.findEntity(preserved) != entt::null),
                  "failed transitions preserve the active scene");
    session.shutdown();
}

TEST_CASE("ApplicationLifecycle: a failed first load clears partial state and stops") {
    RecordingState state;
    FailingApplication application(state);
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    const SceneResult result = session.start();
    CHECK_MESSAGE((!result && result.error == "expected load failure"),
                  "scene load failures preserve their diagnostic");
    CHECK_MESSAGE(entityCount(scene) == 0, "failed scene loads clear partial ECS state");
    CHECK_MESSAGE((state.events == std::vector<std::string>({"failing-load", "failing-unload", "failing-stop"})),
                  "failed loads receive unload and application stop callbacks");
}

TEST_CASE("ApplicationLifecycle: the demo loads its player turns its camera and reloads") {
    DemoApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    CHECK_MESSAGE((static_cast<bool>(session.start())), "demo application loads its gameplay scene");

    const auto models = scene.registry().view<ModelComponent>();
    CHECK_MESSAGE(models.begin() != models.end(), "gameplay scene instantiates its model");
    const ModelHandle firstModel =
        models.begin() != models.end() ? models.get<ModelComponent>(*models.begin()).model : ModelHandle{};
    const auto controllers = scene.registry().view<CharacterControllerComponent>();
    CHECK_MESSAGE(std::distance(controllers.begin(), controllers.end()) == 1, "gameplay scene creates one player");
    const Entity camera =
        controllers.begin() != controllers.end() ? findCameraChild(scene, *controllers.begin()) : entt::null;
    CHECK_MESSAGE((camera != entt::null && scene.registry().all_of<FirstPersonCameraComponent>(camera)),
                  "the spawned player has a first-person camera");

    InputState startGame;
    startGame.setKeyPressed(Key::Space);
    session.update(startGame, 1.0f / 60.0f);
    session.drawUi();

    InputState look;
    look.mouseDelta = {100.0f, 20.0f};
    session.update(look, 1.0f / 60.0f);
    session.lateUpdate(look, 1.0f / 60.0f);
    CHECK_MESSAGE((camera != entt::null && scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians < 0.0f &&
                   scene.registry().get<FirstPersonCameraComponent>(camera).pitchRadians < 0.0f),
                  "the player's camera maps mouse input to view rotation");

    InputState reload;
    reload.setKeyPressed(Key::R);
    session.update(reload, 1.0f / 60.0f);
    CHECK_MESSAGE((static_cast<bool>(session.finishFrame())), "R reloads the gameplay scene");
    const auto reloadedModels = scene.registry().view<ModelComponent>();
    const ModelHandle secondModel = reloadedModels.begin() != reloadedModels.end()
                                        ? reloadedModels.get<ModelComponent>(*reloadedModels.begin()).model
                                        : ModelHandle{};
    CHECK_MESSAGE(firstModel == secondModel, "scene reload reuses the cached model asset");
    session.shutdown();
}

} // namespace

TEST_SUITE_END();
