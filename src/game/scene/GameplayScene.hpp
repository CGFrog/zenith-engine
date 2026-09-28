#pragma once

#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/application/GameSettings.hpp"
#include "game/input/DemoInput.hpp"
#include "game/player/FirstPersonController.hpp"

#include <optional>

class GameplayScene final : public ApplicationScene {
  public:
    GameplayScene(AudioClipHandle interactionSound, std::shared_ptr<InputActionMap> inputActions,
                  std::shared_ptr<GameSettings> settings, std::filesystem::path settingsPath,
                  std::optional<ProjectConfig> project = std::nullopt);
    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    bool fixedUpdatesEnabled() const override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

  private:
    enum class Screen {
        Title,
        Playing,
        Paused,
        Settings,
    };

    void drawHud(GameUiContext& ui) const;
    void drawTitle(ApplicationContext& context);
    void drawPause(ApplicationContext& context);
    void drawSettings(ApplicationContext& context);
    void persistSettings() const;

    FirstPersonControllerSystem m_firstPersonControllers;
    AudioClipHandle m_interactionSound;
    std::shared_ptr<InputActionMap> m_inputActions;
    std::shared_ptr<GameSettings> m_settings;
    std::filesystem::path m_settingsPath;
    std::optional<ProjectConfig> m_project;
    InputActionState m_actions;
    Screen m_screen{Screen::Title};
    Screen m_settingsReturnScreen{Screen::Title};
};
