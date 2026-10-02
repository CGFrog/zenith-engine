#pragma once

#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/application/GameSettings.hpp"
#include "game/input/DemoInput.hpp"
#include "game/scene/CaptureCamera.hpp"

#include <optional>

enum class DemoSceneId : uint32_t {
    Gameplay,
};

constexpr SceneKey demoSceneKey(DemoSceneId scene) {
    return {static_cast<uint32_t>(scene)};
}

class DemoApplication final : public Application {
  public:
    // A capture camera replaces the player and makes the run independent of
    // the persisted game settings: the project's render settings apply
    // unchanged and nothing is loaded from or saved to settingsPath.
    explicit DemoApplication(std::optional<ProjectConfig> project = std::nullopt,
                             std::filesystem::path settingsPath = {},
                             std::optional<CaptureCameraPath> captureCamera = std::nullopt);

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
    std::optional<CaptureCameraPath> m_captureCamera;
};
