#pragma once

#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <functional>

// The editor executable's application: one EditorSession for the project.
// The project's game module (ProjectConfig::gameModule) is loaded by the
// session, which passes its component types to initialSceneLoader.
class EditorApplication final : public Application {
  public:
    using SceneLoader = std::function<SceneResult(ApplicationContext&, const SceneDocumentOptions&)>;

    EditorApplication(std::filesystem::path settingsDirectory, std::filesystem::path fontPath, ProjectConfig project,
                      SceneLoader initialSceneLoader = {});

    MouseCapturePolicy mouseCapturePolicy() const override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;

  private:
    std::filesystem::path m_settingsDirectory;
    std::filesystem::path m_fontPath;
    ProjectConfig m_project;
    SceneLoader m_initialSceneLoader;
};
