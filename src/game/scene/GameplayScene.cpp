#include "GameplayScene.hpp"

#include "DemoScene.hpp"

#include "game/application/DemoApplication.hpp"

#include <SDL3/SDL_stdinc.h>
#include <charconv>
#include <glm/gtc/matrix_transform.hpp>
#include <optional>
#include <string_view>

namespace {
std::optional<uint32_t> environmentCount(const char* name, uint32_t maximum) {
    const char* value = SDL_getenv(name);
    if (!value) {
        return std::nullopt;
    }
    uint32_t count = 0;
    const char* end = value + std::string_view(value).size();
    const auto [position, error] = std::from_chars(value, end, count);
    return error == std::errc{} && position == end ? std::min(count, maximum) : 0u;
}

uint32_t benchmarkObjectCount() {
    return environmentCount("ZENITH_BENCHMARK_OBJECTS", 10000).value_or(0);
}

// Object benchmarks bring 96 local lights unless ZENITH_BENCHMARK_LIGHTS
// sets the count, which also works without benchmark objects.
uint32_t benchmarkLightCount(uint32_t objectCount) {
    constexpr uint32_t objectBenchmarkLights = 96;
    return environmentCount("ZENITH_BENCHMARK_LIGHTS", 8192).value_or(objectCount > 0 ? objectBenchmarkLights : 0);
}
} // namespace

GameplayScene::GameplayScene(AudioClipHandle interactionSound, std::shared_ptr<InputActionMap> inputActions,
                             std::shared_ptr<GameSettings> settings, std::filesystem::path settingsPath,
                             std::optional<ProjectConfig> project)
    : m_interactionSound(interactionSound)
    , m_inputActions(std::move(inputActions))
    , m_settings(std::move(settings))
    , m_settingsPath(std::move(settingsPath))
    , m_project(std::move(project)) {}

SceneResult GameplayScene::load(ApplicationContext& context) {
    Scene& scene = context.scene;
    if (m_project) {
        const ProjectConfigResult result = loadProjectStartupScene(*m_project, context.assets, scene);
        if (!result) {
            return {.error = result.error};
        }
    } else {
        const uint32_t objectCount = benchmarkObjectCount();
        if (const SceneResult result =
                populateDemoScene(scene, context.assets, objectCount, benchmarkLightCount(objectCount));
            !result) {
            return result;
        }
    }

    const Entity player = scene.createEntity("Player");
    scene.registry().get<TransformComponent>(player).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, 4.0f));
    CharacterControllerComponent controller;
    const Entity camera = scene.createEntity("Player Camera");
    controller.camera = camera;
    scene.registry().emplace<CharacterControllerComponent>(player, controller);
    scene.registry().emplace<ColliderComponent>(player, CapsuleCollider{controller.halfHeight, controller.radius});
    RigidBodyComponent playerBody{};
    playerBody.motionType = PhysicsMotionType::Dynamic;
    playerBody.layer = PhysicsLayer::Player;
    playerBody.mass = 80.0f;
    playerBody.friction = 0.0f;
    playerBody.angularDamping = 1.0f;
    playerBody.continuousCollision = true;
    playerBody.lockRotation = true;
    scene.registry().emplace<RigidBodyComponent>(player, playerBody);

    scene.registry().emplace<CameraComponent>(camera);
    scene.registry().emplace<AudioListenerComponent>(camera);
    scene.setParent(camera, player);
    scene.registry().get<TransformComponent>(camera).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f));
    return {.success = true};
}

void GameplayScene::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    m_actions = m_inputActions->evaluate(input);
    if (m_actions.pressed(DemoAction::Pause)) {
        if (m_screen == Screen::Playing) {
            m_screen = Screen::Paused;
        } else if (m_screen == Screen::Paused) {
            m_screen = Screen::Playing;
        } else if (m_screen == Screen::Settings) {
            m_screen = m_settingsReturnScreen;
        }
    }
    if (m_screen != Screen::Playing) {
        return;
    }
    if (m_actions.pressed(DemoAction::Reload)) {
        context.control.requestSceneChange(demoSceneKey(DemoSceneId::Gameplay));
    }
    if (m_interactionSound && m_actions.pressed(DemoAction::Interact)) {
        AudioSpatialOptions options;
        options.position = {0.0f, 1.0f, -2.0f};
        options.gain = 0.8f;
        context.audio.playAt(m_interactionSound, options);
    }
    glm::vec2 look = m_actions.axis2D(DemoAction::Look) * m_settings->lookSensitivity;
    look.y *= m_settings->invertLookY ? -1.0f : 1.0f;
    m_firstPersonControllers.updateView(
        context.scene, {.look = look, .gamepadLook = m_actions.activeDevice() == InputDevice::Gamepad}, deltaSeconds);
}

void GameplayScene::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    const InputActionState actions = m_inputActions->evaluate(input);
    m_firstPersonControllers.fixedUpdate(context.scene, context.physics,
                                         {.move = actions.axis2D(DemoAction::Move),
                                          .jumpPressed = actions.pressed(DemoAction::Jump),
                                          .sprint = actions.down(DemoAction::Sprint)},
                                         deltaSeconds);
}

bool GameplayScene::fixedUpdatesEnabled() const {
    return m_screen == Screen::Playing;
}

void GameplayScene::drawUi(ApplicationContext& context) {
    context.gameUi.setNavigation({.nextPressed = m_actions.pressed(DemoAction::MenuNext),
                                  .previousPressed = m_actions.pressed(DemoAction::MenuPrevious),
                                  .activatePressed = m_actions.pressed(DemoAction::MenuAccept)});
    switch (m_screen) {
    case Screen::Title:
        drawTitle(context);
        break;
    case Screen::Playing:
        drawHud(context.gameUi);
        break;
    case Screen::Paused:
        drawPause(context);
        break;
    case Screen::Settings:
        drawSettings(context);
        break;
    }
}

void GameplayScene::drawHud(GameUiContext& ui) const {
    ui.panel({{-2.0f, -18.0f}, {4.0f, 36.0f}}, {.color = {1.0f, 1.0f, 1.0f, 0.85f}}, UiAnchor::Center);
    ui.panel({{-18.0f, -2.0f}, {36.0f, 4.0f}}, {.color = {1.0f, 1.0f, 1.0f, 0.85f}}, UiAnchor::Center);
    ui.text({24.0f, -48.0f}, "WASD / left stick to move | Escape / Start to pause", 20.0f, {1.0f, 1.0f, 1.0f, 0.8f},
            UiAnchor::BottomLeft);
}

void GameplayScene::drawTitle(ApplicationContext& context) {
    GameUiContext& ui = context.gameUi;
    ui.panel({{-360.0f, -300.0f}, {720.0f, 600.0f}}, {.color = {0.025f, 0.035f, 0.055f, 0.94f}, .cornerRadius = 18.0f},
             UiAnchor::Center);
    ui.text({-128.0f, -220.0f}, "ZENITH", 64.0f, {0.55f, 0.8f, 1.0f, 1.0f}, UiAnchor::Center);
    if (ui.button("title.play", {{-220.0f, -80.0f}, {440.0f, 72.0f}}, "Play", UiAnchor::Center)) {
        m_screen = Screen::Playing;
    }
    if (ui.button("title.settings", {{-220.0f, 20.0f}, {440.0f, 72.0f}}, "Settings", UiAnchor::Center)) {
        m_settingsReturnScreen = Screen::Title;
        m_screen = Screen::Settings;
    }
    if (ui.button("title.quit", {{-220.0f, 120.0f}, {440.0f, 72.0f}}, "Quit", UiAnchor::Center)) {
        context.control.requestQuit();
    }
}

void GameplayScene::drawPause(ApplicationContext& context) {
    GameUiContext& ui = context.gameUi;
    ui.panel({{-320.0f, -270.0f}, {640.0f, 540.0f}}, {.color = {0.025f, 0.035f, 0.055f, 0.94f}, .cornerRadius = 18.0f},
             UiAnchor::Center);
    ui.text({-95.0f, -205.0f}, "Paused", 48.0f, glm::vec4(1.0f), UiAnchor::Center);
    if (ui.button("pause.resume", {{-200.0f, -80.0f}, {400.0f, 70.0f}}, "Resume", UiAnchor::Center)) {
        m_screen = Screen::Playing;
    }
    if (ui.button("pause.settings", {{-200.0f, 10.0f}, {400.0f, 70.0f}}, "Settings", UiAnchor::Center)) {
        m_settingsReturnScreen = Screen::Paused;
        m_screen = Screen::Settings;
    }
    if (ui.button("pause.quit", {{-200.0f, 100.0f}, {400.0f, 70.0f}}, "Quit", UiAnchor::Center)) {
        context.control.requestQuit();
    }
}

void GameplayScene::drawSettings(ApplicationContext& context) {
    GameUiContext& ui = context.gameUi;
    ui.panel({{-380.0f, -520.0f}, {760.0f, 1040.0f}}, {.color = {0.025f, 0.035f, 0.055f, 0.96f}, .cornerRadius = 18.0f},
             UiAnchor::Center);
    ui.text({-105.0f, -465.0f}, "Settings", 44.0f, glm::vec4(1.0f), UiAnchor::Center);
    RenderSettings settings = context.rendering.settings();
    const std::string aaLabel = settings.antialiasing == AntialiasingMode::Off    ? "Anti-aliasing: Off"
                                : settings.antialiasing == AntialiasingMode::Fxaa ? "Anti-aliasing: FXAA"
                                                                                  : "Anti-aliasing: TAA";
    if (ui.button("settings.aa", {{-250.0f, -140.0f}, {500.0f, 68.0f}}, aaLabel, UiAnchor::Center)) {
        settings.antialiasing = static_cast<AntialiasingMode>((static_cast<uint32_t>(settings.antialiasing) + 1) %
                                                              (static_cast<uint32_t>(AntialiasingMode::Taa) + 1));
        context.rendering.setSettings(settings);
        m_settings->antialiasing = settings.antialiasing;
        persistSettings();
    }
    const std::string bloomLabel = settings.bloom == BloomQuality::Off ? "Bloom: Off" : "Bloom: Downsampled";
    if (ui.button("settings.bloom", {{-250.0f, -52.0f}, {500.0f, 68.0f}}, bloomLabel, UiAnchor::Center)) {
        settings.bloom = settings.bloom == BloomQuality::Off ? BloomQuality::Downsampled : BloomQuality::Off;
        context.rendering.setSettings(settings);
        m_settings->bloom = settings.bloom;
        persistSettings();
    }
    const bool muted = context.audio.busMuted(AudioBus::Master);
    if (ui.button("settings.audio", {{-250.0f, 36.0f}, {500.0f, 68.0f}}, muted ? "Audio: Muted" : "Audio: On",
                  UiAnchor::Center)) {
        context.audio.setBusMuted(AudioBus::Master, !muted);
        m_settings->masterMuted = !muted;
        persistSettings();
    }
    const std::string sensitivityLabel = m_settings->lookSensitivity < 0.9f   ? "Look sensitivity: Low"
                                         : m_settings->lookSensitivity > 1.1f ? "Look sensitivity: High"
                                                                              : "Look sensitivity: Normal";
    if (ui.button("settings.sensitivity", {{-250.0f, 124.0f}, {500.0f, 68.0f}}, sensitivityLabel, UiAnchor::Center)) {
        m_settings->lookSensitivity = m_settings->lookSensitivity < 0.9f   ? 1.0f
                                      : m_settings->lookSensitivity < 1.1f ? 1.5f
                                                                           : 0.65f;
        persistSettings();
    }
    if (ui.button("settings.invert", {{-250.0f, 212.0f}, {500.0f, 68.0f}},
                  m_settings->invertLookY ? "Invert look: On" : "Invert look: Off", UiAnchor::Center)) {
        m_settings->invertLookY = !m_settings->invertLookY;
        persistSettings();
    }
    if (ui.button("settings.contrast", {{-250.0f, 300.0f}, {500.0f, 68.0f}},
                  m_settings->accessibility.highContrast ? "High contrast: On" : "High contrast: Off",
                  UiAnchor::Center)) {
        m_settings->accessibility.highContrast = !m_settings->accessibility.highContrast;
        ui.setAccessibility(m_settings->accessibility);
        persistSettings();
    }
    if (ui.button("settings.motion", {{-250.0f, 388.0f}, {500.0f, 68.0f}},
                  m_settings->accessibility.reducedMotion ? "Reduced motion: On" : "Reduced motion: Off",
                  UiAnchor::Center)) {
        m_settings->accessibility.reducedMotion = !m_settings->accessibility.reducedMotion;
        ui.setAccessibility(m_settings->accessibility);
        persistSettings();
    }
    if (ui.button("settings.back", {{-250.0f, 476.0f}, {500.0f, 68.0f}}, "Back", UiAnchor::Center)) {
        m_screen = m_settingsReturnScreen;
    }
}

void GameplayScene::persistSettings() const {
    if (!m_settingsPath.empty()) {
        saveGameSettings(m_settingsPath, *m_settings);
    }
}

void GameplayScene::unload(ApplicationContext&) {}
