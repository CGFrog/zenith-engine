#pragma once

#include "engine/application/Application.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/application/GameSettings.hpp"
#include "game/input/DemoInput.hpp"
#include "game/scene/CaptureCamera.hpp"

#include <optional>

class GameplayScene final : public ApplicationScene {
  public:
    // gameplay supplies the component types scenes load with and the systems
    // the scene runs while playing. With a project the scene plays its
    // startupScene, which need not lie under its scene root (see
    // loadProjectSceneFile); without one it builds the demo room.
    // The scene's menus and its systems share gameplay's input actions.
    GameplayScene(Gameplay gameplay, AudioClipHandle interactionSound, std::shared_ptr<GameSettings> settings,
                  std::filesystem::path settingsPath, std::optional<ProjectConfig> project = std::nullopt,
                  std::optional<CaptureCameraPath> captureCamera = std::nullopt);
    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void afterPhysicsStep(ApplicationContext& context) override;
    void lateUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
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

    void drawHud(GameUiContext& ui, const Scene& scene) const;
    void drawTitle(ApplicationContext& context);
    void drawPause(ApplicationContext& context);
    void drawSettings(ApplicationContext& context);
    void persistSettings() const;
    void spawnPlayer(Scene& scene);
    // Gives authored controllers what spawnPlayer would: the capsule collider
    // and player body they lack, and an audio listener on a camera child when
    // the scene has none.
    void completeAuthoredPlayers(Scene& scene);

    void applyViewPreferences(Scene& scene) const;

    GameplayRunner m_gameplay;
    AudioClipHandle m_interactionSound;
    std::shared_ptr<GameSettings> m_settings;
    std::filesystem::path m_settingsPath;
    std::optional<ProjectConfig> m_project;
    InputActionState m_actions;
    Screen m_screen{Screen::Title};
    Screen m_settingsReturnScreen{Screen::Title};
    // When set, a scripted camera replaces the player: input, menus, and the
    // HUD are ignored so captures see only the scene.
    std::optional<CaptureCameraPath> m_captureCamera;
    Entity m_captureCameraEntity{entt::null};
    float m_captureElapsedSeconds{0.0f};
};
