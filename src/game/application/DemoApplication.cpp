#include "DemoApplication.hpp"

#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "game/application/LinkedGameplay.hpp"
#include "game/scene/GameplayScene.hpp"

#include <filesystem>
#include <iostream>

DemoApplication::DemoApplication(std::optional<ProjectConfig> project, std::filesystem::path settingsPath,
                                 std::optional<CaptureCameraPath> captureCamera, Gameplay gameplay)
    : m_gameplay(std::move(gameplay))
    , m_inputActions(createDemoInputMap())
    , m_settings(std::make_shared<GameSettings>())
    , m_settingsPath(captureCamera ? std::filesystem::path{} : std::move(settingsPath))
    , m_project(std::move(project))
    , m_captureCamera(captureCamera) {}

SceneResult DemoApplication::onStart(ApplicationContext& context) {
    if (!m_gameplay) {
        if (const GameplayResult result = linkDemoGameplay(m_gameplay); !result) {
            return {.error = result.error};
        }
    }
    if (!m_settingsPath.empty() && std::filesystem::exists(m_settingsPath)) {
        if (const GameSettingsResult result = loadGameSettings(m_settingsPath, *m_settings); !result) {
            return {.error = result.error};
        }
    }
    RenderSettings renderSettings = m_project ? m_project->renderSettings : context.rendering.settings();
    if (!m_captureCamera) {
        renderSettings.antialiasing = m_settings->antialiasing;
        renderSettings.bloom = m_settings->bloom;
    }
    ValidatedRenderSettings validated = validateRenderSettings(renderSettings, context.rendering.capabilities());
    for (const std::string& fallback : validated.fallbacks) {
        std::cerr << "Render setting fallback: " << fallback << '\n';
    }
    context.rendering.setSettings(validated.settings);
    if (m_project) {
        // Asset paths in the project's render settings (the colour-grading
        // LUT) are relative to its asset root.
        context.rendering.setAssetRoot(m_project->assetRoot);
        if (const PhysicsResult result = context.physics.setLayers(m_project->physicsLayerTable()); !result) {
            return {.error = "Invalid project physics layers: " + result.error};
        }
    }
    context.audio.setBusMuted(AudioBus::Master, m_settings->masterMuted);
    context.gameUi.setAccessibility(m_settings->accessibility);

    const std::filesystem::path assets = m_project ? m_project->assetRoot : ::assetRoot();
    const auto effect = context.audio.loadClip(assets / "audio" / "interaction.wav", true);
    if (!effect) {
        if (!m_project) {
            return {.error = effect.error};
        }
    } else {
        m_interactionSound = effect.handle;
    }

    const auto music = context.audio.loadStream(assets / "audio" / "ambient.ogg");
    if (!music) {
        if (!m_project) {
            return {.error = music.error};
        }
        return {.success = true};
    }
    AudioPlaybackOptions musicOptions;
    musicOptions.bus = AudioBus::Music;
    musicOptions.scope = AudioScope::Persistent;
    musicOptions.gain = 0.35f;
    musicOptions.loop = true;
    const auto voice = context.audio.playMusic(music.handle, musicOptions);
    if (!voice) {
        return {.error = voice.error};
    }
    m_backgroundMusic = voice.handle;
    return {.success = true};
}

SceneKey DemoApplication::initialScene() const {
    return demoSceneKey(DemoSceneId::Gameplay);
}

std::unique_ptr<ApplicationScene> DemoApplication::createScene(SceneKey scene) {
    if (scene == demoSceneKey(DemoSceneId::Gameplay)) {
        return std::make_unique<GameplayScene>(m_gameplay, m_interactionSound, m_inputActions, m_settings,
                                               m_settingsPath, m_project, m_captureCamera);
    }

    return nullptr;
}

void DemoApplication::onStop(ApplicationContext& context) {
    if (context.audio.isPlaying(m_backgroundMusic)) {
        context.audio.stop(m_backgroundMusic);
    }
    m_backgroundMusic = {};
}
