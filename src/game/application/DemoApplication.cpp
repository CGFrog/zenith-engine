#include "DemoApplication.hpp"

#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "game/scene/GameplayScene.hpp"

#include <filesystem>
#include <iostream>

DemoApplication::DemoApplication(std::optional<ProjectConfig> project, std::filesystem::path settingsPath)
    : m_inputActions(createDemoInputMap())
    , m_settings(std::make_shared<GameSettings>())
    , m_settingsPath(std::move(settingsPath))
    , m_project(std::move(project)) {}

SceneResult DemoApplication::onStart(ApplicationContext& context) {
    if (!m_settingsPath.empty() && std::filesystem::exists(m_settingsPath)) {
        if (const GameSettingsResult result = loadGameSettings(m_settingsPath, *m_settings); !result) {
            return {.error = result.error};
        }
    }
    RenderSettings renderSettings = m_project ? m_project->renderSettings : context.rendering.settings();
    renderSettings.antialiasing = m_settings->antialiasing;
    renderSettings.bloom = m_settings->bloom;
    ValidatedRenderSettings validated = validateRenderSettings(renderSettings, context.rendering.capabilities());
    for (const std::string& fallback : validated.fallbacks) {
        std::cerr << "Render setting fallback: " << fallback << '\n';
    }
    context.rendering.setSettings(validated.settings);
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
        return std::make_unique<GameplayScene>(m_interactionSound, m_inputActions, m_settings, m_settingsPath,
                                               m_project);
    }

    return nullptr;
}

void DemoApplication::onStop(ApplicationContext& context) {
    if (context.audio.isPlaying(m_backgroundMusic)) {
        context.audio.stop(m_backgroundMusic);
    }
    m_backgroundMusic = {};
}
