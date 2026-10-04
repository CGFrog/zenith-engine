#include "ProjectApplication.hpp"

#include "engine/graphics/RenderConfiguration.hpp"
#include "game/application/GameLaunch.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {
constexpr SceneKey projectSceneKey{0};

std::string playSystemList(const SystemScheduler& systems) {
    std::string names;
    for (const SystemPhase phase : {SystemPhase::Update, SystemPhase::FixedUpdate}) {
        for (const std::string& name : systems.schedule(phase, SimulationMode::Play)) {
            names += (names.empty() ? "" : ", ") + name;
        }
    }
    return names.empty() ? "none" : names;
}

class ProjectPlayScene final : public ApplicationScene {
  public:
    ProjectPlayScene(ProjectConfig project, Gameplay gameplay, std::optional<CaptureCameraPath> captureCamera)
        : m_project(std::move(project))
        , m_gameplay(std::move(gameplay))
        , m_captureCamera(captureCamera) {}

    SceneResult load(ApplicationContext& context) override {
        if (!m_gameplay) {
            return {.error = "Project scene was created without gameplay"};
        }
        if (const ProjectConfigResult result = loadProjectSceneFile(m_project, m_project.startupScene, context.assets,
                                                                    context.scene, m_gameplay.componentTypes);
            !result) {
            return {.error = result.error};
        }
        if (m_captureCamera) {
            m_captureCameraEntity = spawnCaptureCamera(context.scene, *m_captureCamera);
        }
        context.logger.log(LogLevel::Info, "game",
                           "Playing " + utf8FromPath(m_project.startupScene.filename()) +
                               " with Play systems: " + playSystemList(*m_gameplay.systems));
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override {
        if (m_captureCamera) {
            // Sampled after each step, so frame N shows N * deltaSeconds of
            // motion, as in the demo.
            m_captureElapsedSeconds += deltaSeconds;
            moveCaptureCamera(context.scene, m_captureCameraEntity, *m_captureCamera, m_captureElapsedSeconds);
            return;
        }
        m_gameplay.play(SystemPhase::Update, context, input, deltaSeconds);
    }

    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override {
        if (m_captureCamera) {
            return;
        }
        m_gameplay.play(SystemPhase::FixedUpdate, context, input, deltaSeconds);
    }

    void unload(ApplicationContext&) override {}

  private:
    ProjectConfig m_project;
    Gameplay m_gameplay;
    std::optional<CaptureCameraPath> m_captureCamera;
    Entity m_captureCameraEntity{entt::null};
    float m_captureElapsedSeconds{0.0f};
};
} // namespace

ProjectApplication::ProjectApplication(ProjectConfig project, Gameplay gameplay,
                                       std::optional<CaptureCameraPath> captureCamera)
    : m_project(std::move(project))
    , m_gameplay(std::move(gameplay))
    , m_captureCamera(captureCamera) {}

SceneResult ProjectApplication::onStart(ApplicationContext& context) {
    const ValidatedRenderSettings validated =
        validateRenderSettings(m_project.renderSettings, context.rendering.capabilities());
    for (const std::string& fallback : validated.fallbacks) {
        context.logger.log(LogLevel::Warning, "game", "Render setting fallback: " + fallback);
    }
    context.rendering.setSettings(validated.settings);
    // Asset paths in the render settings (the colour-grading LUT) are
    // relative to the project's asset root.
    context.rendering.setAssetRoot(m_project.assetRoot);
    return {.success = true};
}

SceneKey ProjectApplication::initialScene() const {
    return projectSceneKey;
}

std::unique_ptr<ApplicationScene> ProjectApplication::createScene(SceneKey scene) {
    if (scene != projectSceneKey) {
        return nullptr;
    }
    return std::make_unique<ProjectPlayScene>(m_project, m_gameplay, m_captureCamera);
}
