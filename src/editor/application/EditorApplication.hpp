#pragma once

#include "editor/project/ProjectConfig.hpp"
#include "engine/application/Application.hpp"

#include <filesystem>
#include <functional>

class EditorApplication final : public Application {
  public:
    EditorApplication(std::filesystem::path settingsDirectory, ProjectConfig project,
                      std::function<SceneResult(ApplicationContext&)> initialSceneLoader = {});

    MouseCapturePolicy mouseCapturePolicy() const override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;

  private:
    std::filesystem::path m_settingsDirectory;
    ProjectConfig m_project;
    std::function<SceneResult(ApplicationContext&)> m_initialSceneLoader;
};
