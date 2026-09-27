#pragma once

#include "engine/application/Application.hpp"
#include "game/player/FirstPersonController.hpp"

class GameplayScene final : public ApplicationScene {
  public:
    explicit GameplayScene(AudioClipHandle interactionSound);
    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

  private:
    FirstPersonControllerSystem m_firstPersonControllers;
    AudioClipHandle m_interactionSound;
};
