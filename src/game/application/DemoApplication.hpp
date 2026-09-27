#pragma once

#include "engine/application/Application.hpp"

enum class DemoSceneId : uint32_t {
    Gameplay,
};

constexpr SceneKey demoSceneKey(DemoSceneId scene) {
    return {static_cast<uint32_t>(scene)};
}

class DemoApplication final : public Application {
  public:
    SceneResult onStart(ApplicationContext& context) override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;
    void onStop(ApplicationContext& context) override;

  private:
    AudioClipHandle m_interactionSound;
    AudioVoiceHandle m_backgroundMusic;
};
