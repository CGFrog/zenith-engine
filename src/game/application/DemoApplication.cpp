#include "DemoApplication.hpp"

#include "game/scene/GameplayScene.hpp"

#include <filesystem>

SceneResult DemoApplication::onStart(ApplicationContext& context) {
    const auto effect =
        context.audio.loadClip(std::filesystem::path{ZENITH_ASSET_DIR} / "audio" / "interaction.wav", true);
    if (!effect) {
        return {.error = effect.error};
    }
    m_interactionSound = effect.handle;

    const auto music = context.audio.loadStream(std::filesystem::path{ZENITH_ASSET_DIR} / "audio" / "ambient.ogg");
    if (!music) {
        return {.error = music.error};
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
        return std::make_unique<GameplayScene>(m_interactionSound);
    }

    return nullptr;
}

void DemoApplication::onStop(ApplicationContext& context) {
    if (context.audio.isPlaying(m_backgroundMusic)) {
        context.audio.stop(m_backgroundMusic);
    }
    m_backgroundMusic = {};
}
