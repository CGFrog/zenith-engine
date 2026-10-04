#include "GameplayScene.hpp"

#include "DemoScene.hpp"

#include "engine/gameplay/FirstPerson.hpp"
#include "game/application/DemoApplication.hpp"
#include "game/gameplay/ItemCollection.hpp"

#include <SDL3/SDL_stdinc.h>
#include <charconv>
#include <glm/gtc/matrix_transform.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

GameplayScene::GameplayScene(Gameplay gameplay, AudioClipHandle interactionSound,
                             std::shared_ptr<GameSettings> settings, std::filesystem::path settingsPath,
                             std::optional<ProjectConfig> project, std::optional<CaptureCameraPath> captureCamera)
    : m_gameplay(std::move(gameplay))
    , m_interactionSound(interactionSound)
    , m_settings(std::move(settings))
    , m_settingsPath(std::move(settingsPath))
    , m_project(std::move(project))
    , m_captureCamera(captureCamera) {}

SceneResult GameplayScene::load(ApplicationContext& context) {
    const Gameplay& gameplay = m_gameplay.gameplay();
    if (!gameplay) {
        return {.error = "Gameplay scene was created without gameplay"};
    }
    Scene& scene = context.scene;
    if (m_project) {
        const ProjectConfigResult result =
            loadProjectSceneFile(*m_project, m_project->startupScene, context.assets, scene, gameplay.componentTypes);
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

    if (m_captureCamera) {
        m_captureCameraEntity = spawnCaptureCamera(scene, *m_captureCamera);
        m_captureElapsedSeconds = 0.0f;
        m_screen = Screen::Playing;
    } else if (scene.registry().view<CharacterControllerComponent>().empty()) {
        spawnFirstPersonPlayer(scene, {0.0f, 1.2f, 4.0f});
    } else {
        // A scene authored with its own character controller keeps it.
        completeAuthoredPlayers(scene);
    }
    if (!m_captureCamera) {
        m_gameplay.start(context, m_project ? m_project->assetRoot : std::filesystem::path{});
    }
    return {.success = true};
}

void GameplayScene::completeAuthoredPlayers(Scene& scene) {
    entt::registry& registry = scene.registry();
    if (!registry.view<AudioListenerComponent>().empty()) {
        return;
    }
    for (const Entity entity : registry.view<CharacterControllerComponent>()) {
        if (const Entity camera = findCameraChild(scene, entity); camera != entt::null) {
            registry.emplace<AudioListenerComponent>(camera);
            return;
        }
    }
}

void GameplayScene::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    if (m_captureCamera) {
        // The path is sampled at the end of each frame's step, so frame N
        // shows N * deltaSeconds of motion.
        m_captureElapsedSeconds += deltaSeconds;
        moveCaptureCamera(context.scene, m_captureCameraEntity, *m_captureCamera, m_captureElapsedSeconds);
        return;
    }
    // The menus read the actions directly; Play systems get them from the
    // runner, which evaluates the same map.
    m_actions = m_gameplay.actionMap().evaluate(input);
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
    applyViewPreferences(context.scene);
    m_gameplay.update(context, input, deltaSeconds);
}

void GameplayScene::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    if (m_captureCamera) {
        return;
    }
    m_gameplay.fixedUpdate(context, input, deltaSeconds);
}

void GameplayScene::afterPhysicsStep(ApplicationContext& context) {
    if (!m_captureCamera) {
        m_gameplay.afterPhysicsStep(context);
    }
}

void GameplayScene::lateUpdate(ApplicationContext& context, const InputState&, float) {
    if (!m_captureCamera && m_screen == Screen::Playing) {
        m_gameplay.lateUpdate(context);
    }
}

void GameplayScene::applyViewPreferences(Scene& scene) const {
    scene.registry().ctx().insert_or_assign(FirstPersonViewPreferences{
        .lookScale = m_settings->lookSensitivity,
        .invertLookY = m_settings->invertLookY,
    });
}

bool GameplayScene::fixedUpdatesEnabled() const {
    return m_screen == Screen::Playing;
}

void GameplayScene::drawUi(ApplicationContext& context) {
    if (m_captureCamera) {
        return;
    }
    context.gameUi.setNavigation({.nextPressed = m_actions.pressed(DemoAction::MenuNext),
                                  .previousPressed = m_actions.pressed(DemoAction::MenuPrevious),
                                  .activatePressed = m_actions.pressed(DemoAction::MenuAccept),
                                  .leftPressed = m_actions.pressed(DemoAction::MenuLeft),
                                  .rightPressed = m_actions.pressed(DemoAction::MenuRight)});
    switch (m_screen) {
    case Screen::Title:
        drawTitle(context);
        break;
    case Screen::Playing:
        drawHud(context.gameUi, context.scene);
        m_gameplay.drawUi(context);
        break;
    case Screen::Paused:
        drawPause(context);
        break;
    case Screen::Settings:
        drawSettings(context);
        break;
    }
}

void GameplayScene::drawHud(GameUiContext& ui, const Scene& scene) const {
    const ItemCollectionProgress progress = itemCollectionProgress(scene);
    ui.panel({{-2.0f, -18.0f}, {4.0f, 36.0f}}, {.color = {1.0f, 1.0f, 1.0f, 0.85f}}, UiAnchor::Center);
    ui.panel({{-18.0f, -2.0f}, {36.0f, 4.0f}}, {.color = {1.0f, 1.0f, 1.0f, 0.85f}}, UiAnchor::Center);
    ui.text({-220.0f, 24.0f}, "Items: " + std::to_string(progress.collected) + " / " + std::to_string(progress.total),
            26.0f, {1.0f, 1.0f, 1.0f, 0.95f}, UiAnchor::TopRight);
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
    switch (drawPauseMenu(context.gameUi)) {
    case MenuAction::Resume:
        m_screen = Screen::Playing;
        break;
    case MenuAction::OpenSettings:
        m_settingsReturnScreen = Screen::Paused;
        m_screen = Screen::Settings;
        break;
    case MenuAction::Quit:
        context.control.requestQuit();
        break;
    default:
        break;
    }
}

std::vector<SettingsEntry> GameplayScene::settingsEntries(ApplicationContext& context) {
    const auto renderSetting = [&context, this](auto change) {
        RenderSettings settings = context.rendering.settings();
        change(settings);
        context.rendering.setSettings(settings);
        m_settings->antialiasing = settings.antialiasing;
        m_settings->bloom = settings.bloom;
    };
    const auto accessibility = [&context, this](auto change) {
        change(m_settings->accessibility);
        context.gameUi.setAccessibility(m_settings->accessibility);
    };
    const auto percent = [](float value) { return std::to_string(static_cast<int>(value * 100.0f + 0.5f)) + "%"; };
    return {
        {.id = "aa",
         .label = "Anti-aliasing",
         .control = SettingsChoice{
             .options = {"Off", "FXAA", "TAA"},
             .get = [&context] { return static_cast<size_t>(context.rendering.settings().antialiasing); },
             .set =
                 [renderSetting](size_t option) {
                     renderSetting([option](RenderSettings& settings) {
                         settings.antialiasing = static_cast<AntialiasingMode>(option);
                     });
                 }}},
        {.id = "bloom",
         .label = "Bloom",
         .control =
             SettingsToggle{.get = [&context] { return context.rendering.settings().bloom != BloomQuality::Off; },
                            .set =
                                [renderSetting](bool on) {
                                    renderSetting([on](RenderSettings& settings) {
                                        settings.bloom = on ? BloomQuality::Downsampled : BloomQuality::Off;
                                    });
                                }}},
        {.id = "volume",
         .label = "Volume",
         .control = SettingsSlider{.minimum = 0.0f,
                                   .maximum = 1.0f,
                                   .step = 0.05f,
                                   .get = [this] { return m_settings->masterVolume; },
                                   .set =
                                       [&context, this](float volume) {
                                           m_settings->masterVolume = volume;
                                           context.audio.setBusGain(AudioBus::Master, volume);
                                       },
                                   .format = percent}},
        {.id = "mute",
         .label = "Mute audio",
         .control = SettingsToggle{.get = [this] { return m_settings->masterMuted; },
                                   .set =
                                       [&context, this](bool muted) {
                                           m_settings->masterMuted = muted;
                                           context.audio.setBusMuted(AudioBus::Master, muted);
                                       }}},
        {.id = "sensitivity",
         .label = "Look sensitivity",
         .control = SettingsSlider{.minimum = 0.25f,
                                   .maximum = 4.0f,
                                   .step = 0.05f,
                                   .get = [this] { return m_settings->lookSensitivity; },
                                   .set = [this](float sensitivity) { m_settings->lookSensitivity = sensitivity; }}},
        {.id = "invert",
         .label = "Invert look",
         .control = SettingsToggle{.get = [this] { return m_settings->invertLookY; },
                                   .set = [this](bool invert) { m_settings->invertLookY = invert; }}},
        {.id = "contrast",
         .label = "High contrast",
         .control = SettingsToggle{.get = [this] { return m_settings->accessibility.highContrast; },
                                   .set =
                                       [accessibility](bool on) {
                                           accessibility(
                                               [on](AccessibilitySettings& settings) { settings.highContrast = on; });
                                       }}},
        {.id = "motion",
         .label = "Reduced motion",
         .control = SettingsToggle{.get = [this] { return m_settings->accessibility.reducedMotion; },
                                   .set =
                                       [accessibility](bool on) {
                                           accessibility(
                                               [on](AccessibilitySettings& settings) { settings.reducedMotion = on; });
                                       }}},
    };
}

void GameplayScene::drawSettings(ApplicationContext& context) {
    const std::vector<SettingsEntry> entries = settingsEntries(context);
    switch (drawSettingsMenu(context.gameUi, entries)) {
    case MenuAction::Changed:
        persistSettings();
        break;
    case MenuAction::Back:
        m_screen = m_settingsReturnScreen;
        break;
    default:
        break;
    }
}

void GameplayScene::persistSettings() const {
    if (!m_settingsPath.empty()) {
        saveGameSettings(m_settingsPath, *m_settings);
    }
}

SimulationClock GameplayScene::simulationClock() const {
    return m_gameplay.simulationClock();
}

void GameplayScene::unload(ApplicationContext& context) {
    m_gameplay.stop(context);
}
