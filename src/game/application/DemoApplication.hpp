#pragma once

#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/application/GameSettings.hpp"
#include "game/input/DemoInput.hpp"

#include <optional>

enum class DemoSceneId : uint32_t {
    Gameplay,
};

constexpr SceneKey demoSceneKey(DemoSceneId scene) {
    return {static_cast<uint32_t>(scene)};
}

class DemoApplication final : public Application {
  public:
    explicit DemoApplication(std::optional<ProjectConfig> project = std::nullopt,
                             std::filesystem::path settingsPath = {});

    SceneResult onStart(ApplicationContext& context) override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;
    void onStop(ApplicationContext& context) override;

  private:
    AudioClipHandle m_interactionSound;
    AudioVoiceHandle m_backgroundMusic;
    std::shared_ptr<InputActionMap> m_inputActions;
    std::shared_ptr<GameSettings> m_settings;
    std::filesystem::path m_settingsPath;
    std::optional<ProjectConfig> m_project;
};
