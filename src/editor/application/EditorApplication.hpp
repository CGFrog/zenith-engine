#pragma once

#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <filesystem>
#include <functional>

class EditorApplication final : public Application {
  public:
    EditorApplication(std::filesystem::path settingsDirectory, std::filesystem::path fontPath, ProjectConfig project,
                      std::function<SceneResult(ApplicationContext&)> initialSceneLoader = {});

    MouseCapturePolicy mouseCapturePolicy() const override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;

  private:
    std::filesystem::path m_settingsDirectory;
    std::filesystem::path m_fontPath;
    ProjectConfig m_project;
    std::function<SceneResult(ApplicationContext&)> m_initialSceneLoader;
};
