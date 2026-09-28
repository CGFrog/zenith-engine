#include "Application.hpp"

#include <utility>

void ApplicationControl::requestSceneChange(SceneKey scene) {
    if (!m_quitRequested && !m_requestedScene) {
        m_requestedScene = scene;
    }
}

void ApplicationControl::requestQuit() {
    m_quitRequested = true;
    m_requestedScene.reset();
}

bool ApplicationControl::actionPending() const {
    return m_quitRequested || m_requestedScene.has_value();
}

void ApplicationControl::clear() {
    m_requestedScene.reset();
    m_quitRequested = false;
}

SceneResult Application::onStart(ApplicationContext&) {
    return {.success = true};
}

MouseCapturePolicy Application::mouseCapturePolicy() const {
    return MouseCapturePolicy::Persistent;
}

void Application::onStop(ApplicationContext&) {}

void ApplicationScene::drawUi(ApplicationContext&) {}

bool ApplicationScene::fixedUpdatesEnabled() const {
    return true;
}

bool ApplicationScene::beginFixedUpdate() {
    return true;
}

void ApplicationScene::requestQuit(ApplicationContext& context) {
    context.control.requestQuit();
}

ApplicationSession::ApplicationSession(Application& application, Scene& scene, AssetManager& assets,
                                       PhysicsWorld& physics, AudioSystem& audio)
    : m_application(application)
    , m_context{scene, assets, physics, audio, m_control, m_fallbackRendering, m_fallbackGameUi} {}

ApplicationSession::ApplicationSession(Application& application, Scene& scene, AssetManager& assets,
                                       PhysicsWorld& physics, AudioSystem& audio, RenderRuntime& rendering)
    : m_application(application)
    , m_context{scene, assets, physics, audio, m_control, rendering, m_fallbackGameUi} {}

ApplicationSession::ApplicationSession(Application& application, Scene& scene, AssetManager& assets,
                                       PhysicsWorld& physics, AudioSystem& audio, RenderRuntime& rendering,
                                       GameUiContext& gameUi)
    : m_application(application)
    , m_context{scene, assets, physics, audio, m_control, rendering, gameUi} {}

ApplicationSession::~ApplicationSession() {
    shutdown();
}

SceneResult ApplicationSession::start() {
    if (m_started) {
        return {.success = true};
    }

    m_started = true;
    if (SceneResult result = m_application.onStart(m_context); !result) {
        const std::string error = result.error;
        shutdown();
        return {.error = error};
    }

    if (SceneResult result = activate(m_application.initialScene()); !result) {
        const std::string error = result.error;
        shutdown();
        return {.error = error};
    }

    return {.success = true};
}

void ApplicationSession::update(const InputState& input, float deltaSeconds) {
    if (m_activeScene) {
        m_activeScene->update(m_context, input, deltaSeconds);
    }
}

void ApplicationSession::fixedUpdate(const InputState& input, float deltaSeconds) {
    if (m_activeScene) {
        m_activeScene->fixedUpdate(m_context, input, deltaSeconds);
    }
}

bool ApplicationSession::fixedUpdatesEnabled() const {
    return m_activeScene && m_activeScene->fixedUpdatesEnabled();
}

bool ApplicationSession::beginFixedUpdate() {
    return m_activeScene && m_activeScene->beginFixedUpdate();
}

void ApplicationSession::drawUi() {
    if (m_activeScene) {
        m_activeScene->drawUi(m_context);
    }
}

ApplicationFrameResult ApplicationSession::finishFrame() {
    if (m_control.m_quitRequested) {
        m_control.clear();
        return {.success = true, .keepRunning = false};
    }

    if (!m_control.m_requestedScene) {
        return {.success = true, .keepRunning = true};
    }

    const SceneKey requested = *m_control.m_requestedScene;
    m_control.clear();
    if (SceneResult result = transition(requested); !result) {
        return {.error = std::move(result.error)};
    }

    return {.success = true, .keepRunning = true};
}

bool ApplicationSession::actionPending() const {
    return m_control.actionPending();
}

void ApplicationSession::requestQuit() {
    if (m_activeScene) {
        m_activeScene->requestQuit(m_context);
    } else {
        m_control.requestQuit();
    }
}

void ApplicationSession::shutdown() {
    if (!m_started) {
        return;
    }

    deactivate();
    m_application.onStop(m_context);
    m_control.clear();
    m_started = false;
}

SceneResult ApplicationSession::activate(SceneKey scene) {
    m_activeScene = m_application.createScene(scene);
    if (!m_activeScene) {
        return {.error = "Application did not create the requested scene"};
    }

    if (SceneResult result = m_activeScene->load(m_context); !result) {
        m_activeScene->unload(m_context);
        m_activeScene.reset();
        m_context.audio.stopSceneVoices();
        m_context.physics.clear();
        m_context.scene.clear();
        return result;
    }

    return {.success = true};
}

SceneResult ApplicationSession::transition(SceneKey scene) {
    std::unique_ptr<ApplicationScene> candidate = m_application.createScene(scene);
    if (!candidate) {
        return {.error = "Application did not create the requested scene"};
    }

    Scene stagedScene;
    ApplicationControl stagedControl;
    ApplicationContext stagedContext{stagedScene,   m_context.assets,    m_context.physics, m_context.audio,
                                     stagedControl, m_context.rendering, m_context.gameUi};
    if (SceneResult result = candidate->load(stagedContext); !result) {
        candidate->unload(stagedContext);
        return result;
    }

    if (m_activeScene) {
        m_activeScene->unload(m_context);
    }
    m_context.audio.stopSceneVoices();
    m_context.physics.clear();
    m_context.scene.replaceWith(std::move(stagedScene));
    m_activeScene = std::move(candidate);
    return {.success = true};
}

void ApplicationSession::deactivate() {
    if (m_activeScene) {
        m_activeScene->unload(m_context);
        m_activeScene.reset();
    }

    m_context.audio.stopSceneVoices();
    m_context.physics.clear();
    m_context.scene.clear();
}
