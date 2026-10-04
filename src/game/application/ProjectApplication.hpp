#pragma once

#include "engine/application/Application.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/scene/CaptureCamera.hpp"

#include <optional>

// The project-agnostic game: plays a project's scene with the project's
// render settings and runs the gameplay's Play systems every frame with the
// player's input. It adds nothing of its own (no menus, player, or audio), so
// a game module decides everything the player sees; it knows nothing about
// the demo.
class ProjectApplication final : public Application {
  public:
    // Plays project.startupScene, which need not lie under its scene root
    // (see loadProjectSceneFile), with gameplay, which must outlive the
    // application's scenes as RuntimeGameplay describes. A capture camera
    // poses the view for deterministic captures, and the gameplay's systems
    // do not run.
    ProjectApplication(ProjectConfig project, Gameplay gameplay,
                       std::optional<CaptureCameraPath> captureCamera = std::nullopt);

    SceneResult onStart(ApplicationContext& context) override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;

  private:
    ProjectConfig m_project;
    Gameplay m_gameplay;
    std::optional<CaptureCameraPath> m_captureCamera;
};
