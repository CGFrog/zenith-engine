#include "EditorApplication.hpp"

#include "EditorSession.hpp"

#include <utility>

namespace {
constexpr SceneKey editorSessionKey{0};
}

EditorApplication::EditorApplication(std::filesystem::path settingsDirectory, ProjectConfig project,
                                     std::function<SceneResult(ApplicationContext&)> initialSceneLoader)
    : m_settingsDirectory(std::move(settingsDirectory))
    , m_project(std::move(project))
    , m_initialSceneLoader(std::move(initialSceneLoader)) {}

MouseCapturePolicy EditorApplication::mouseCapturePolicy() const {
    return MouseCapturePolicy::WhileRightMouseDown;
}

SceneKey EditorApplication::initialScene() const {
    return editorSessionKey;
}

std::unique_ptr<ApplicationScene> EditorApplication::createScene(SceneKey scene) {
    if (scene != editorSessionKey) {
        return nullptr;
    }
    return std::make_unique<EditorSession>(m_settingsDirectory, m_project.assetRoot, m_project.sceneRoot,
                                           m_initialSceneLoader);
}
