#include "EditorApplication.hpp"

#include "EditorSession.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/ThemeChrome.hpp"

#include <imgui.h>

#include <SDL3/SDL_video.h>

#include <chrono>
#include <utility>

namespace {
constexpr SceneKey editorSessionKey{0};
constexpr SceneKey projectHubKey{1};
constexpr const char* logCategory = "Editor";

// The pixel theme on the application's ImGui context: metrics and colours for
// the display's scale, the fonts, popup bevels, and no imgui.ini (each
// workspace keeps its own layout in workspaces.json). Fonts must be added
// before the first frame builds the atlas, so this runs once, before the hub
// or the first session draws.
void installEditorTheme(const std::filesystem::path& fontPath, Logger& logger) {
    ImGuiIO& io = ImGui::GetIO();
    // Size the UI for the display's scale factor so text stays crisp on
    // high-DPI monitors instead of being stretched. The pixel theme rounds it
    // to a whole number of screen pixels per art pixel.
    float contentScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (contentScale <= 0.0f) {
        contentScale = 1.0f;
    }
    const float artPixel = artPixelForScale(contentScale);
    applyEditorMetrics(ImGui::GetStyle(), artPixel);
    applyEditorColors(ImGui::GetStyle());
    const EditorFontResult fonts = loadEditorFonts(io, fontPath.parent_path(), fontPath, artPixel);
    if (!fonts) {
        logger.log(LogLevel::Error, logCategory, fonts.error);
    }
    for (const std::string& warning : fonts.warnings) {
        logger.log(LogLevel::Warning, logCategory, warning);
    }
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    io.IniFilename = nullptr;
}

// Loads the project's startup scene with the editor's document options, which
// carry the game module's component types when the project has a module.
SceneResult loadStartupScene(const ProjectConfig& project, const SceneDocumentOptions& options, AssetManager& assets,
                             Scene& scene) {
    std::filesystem::path scenePath;
    if (const ProjectConfigResult resolved =
            resolveProjectScene(project, project.startupScene.lexically_relative(project.sceneRoot), scenePath);
        !resolved) {
        return {.error = resolved.error};
    }
    const SceneDocumentResult loaded = SceneDocument::load(scenePath, assets, options, scene);
    return {.success = loaded.success, .error = loaded.error};
}

// The project's startup scene, or the default scene when it has none. With
// the hub, a scene that fails to load is logged and replaced by the default
// scene, so a broken scene does not close the editor; automated runs keep the
// failure fatal.
EditorSceneLoader startupSceneLoader(const ProjectConfig& project, bool recoverWithDefaultScene) {
    if (project.startupScene.empty()) {
        return {};
    }
    return [project, recoverWithDefaultScene](ApplicationContext& context, const SceneDocumentOptions& options) {
        SceneResult loaded = loadStartupScene(project, options, context.assets, context.scene);
        if (loaded || !recoverWithDefaultScene) {
            return loaded;
        }
        context.logger.log(LogLevel::Error, logCategory,
                           "Could not open the startup scene of " + project.displayName + ": " + loaded.error);
        context.scene.clear();
        if (SceneResult fallback = populateDefaultScene(context.scene, context.assets, options.assetRoot); !fallback) {
            context.logger.log(LogLevel::Warning, logCategory, fallback.error);
        }
        return SceneResult{.success = true};
    };
}

// The hub as an application scene: no world, only the hub over the wallpaper.
class ProjectHubScene final : public ApplicationScene {
  public:
    ProjectHubScene(EditorApplication& application, RecentProjects& recentProjects, ProjectHubConfig config,
                    ProjectHubEntry entry)
        : m_application(application)
        , m_hub(recentProjects, std::move(config))
        , m_entry(entry) {}

    SceneResult load(ApplicationContext&) override {
        if (m_entry == ProjectHubEntry::OpenProjectDialog) {
            m_hub.browseForProject();
        }
        return {.success = true};
    }

    void update(ApplicationContext&, const InputState&, float) override {}

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    bool fixedUpdatesEnabled() const override {
        return false;
    }

    void drawUi(ApplicationContext& context) override {
        m_chrome.beginFrame(context.rendering);
        const std::optional<std::filesystem::path> chosen = m_hub.draw(context.assets, &m_chrome);
        if (!chosen) {
            return;
        }
        if (const ProjectConfigResult opened = m_application.openProject(context, *chosen); !opened) {
            m_hub.reportError(opened.error);
        }
    }

    void unload(ApplicationContext& context) override {
        m_chrome.release(context.rendering);
    }

  private:
    EditorApplication& m_application;
    ProjectHub m_hub;
    ProjectHubEntry m_entry;
    ThemeChrome m_chrome;
};
} // namespace

EditorApplication::EditorApplication(std::filesystem::path settingsDirectory, std::filesystem::path fontPath,
                                     ProjectConfig project, std::optional<EditorProjectHubOptions> projectHub)
    : m_settingsDirectory(std::move(settingsDirectory))
    , m_fontPath(std::move(fontPath))
    , m_project(std::move(project))
    , m_projectHub(std::move(projectHub))
    , m_recentProjects(m_settingsDirectory / "recent-projects.json") {}

SceneResult EditorApplication::onStart(ApplicationContext& context) {
    installEditorTheme(m_fontPath, context.logger);
    if (const RecentProjectsResult loaded = m_recentProjects.load(); !loaded) {
        context.logger.log(LogLevel::Warning, logCategory, loaded.error);
    }
    if (initialScene() == editorSessionKey) {
        recordRecentProject(context.logger);
    }
    return {.success = true};
}

MouseCapturePolicy EditorApplication::mouseCapturePolicy() const {
    return MouseCapturePolicy::WhileRightMouseDown;
}

SceneKey EditorApplication::initialScene() const {
    return m_projectHub && m_projectHub->startInHub ? projectHubKey : editorSessionKey;
}

std::unique_ptr<ApplicationScene> EditorApplication::createScene(SceneKey scene) {
    if (scene == editorSessionKey) {
        return createSession();
    }
    if (scene == projectHubKey && m_projectHub) {
        return createProjectHub();
    }
    return nullptr;
}

ProjectConfigResult EditorApplication::openProject(ApplicationContext& context,
                                                   const std::filesystem::path& projectFile) {
    ProjectConfig project;
    if (ProjectConfigResult loaded = loadProjectConfig(projectFile, project); !loaded) {
        return loaded;
    }
    m_project = std::move(project);
    recordRecentProject(context.logger);
    context.control.requestSceneChange(editorSessionKey);
    return {.success = true};
}

void EditorApplication::showProjectHub(ApplicationContext& context, ProjectHubEntry entry) {
    if (!m_projectHub) {
        return;
    }
    m_hubEntry = entry;
    context.control.requestSceneChange(projectHubKey);
}

const ProjectConfig& EditorApplication::project() const {
    return m_project;
}

const RecentProjects& EditorApplication::recentProjects() const {
    return m_recentProjects;
}

void EditorApplication::recordRecentProject(Logger& logger) {
    if (m_project.projectFile.empty()) {
        return;
    }
    m_recentProjects.recordOpened(m_project.projectFile, m_project.displayName, std::chrono::system_clock::now());
    if (const RecentProjectsResult saved = m_recentProjects.save(); !saved) {
        logger.log(LogLevel::Warning, logCategory, saved.error);
    }
}

std::unique_ptr<ApplicationScene> EditorApplication::createSession() {
    // Without a project file, render settings persist beside the editor's
    // own settings.
    EditorRenderSettings renderSettings(m_project, m_settingsDirectory / "render-settings.json");
    EditorProjectServices services{.build = projectBuildRequest(m_project.projectFile)};
    if (m_projectHub) {
        services.leaveForProjectHub = [this](ApplicationContext& context, ProjectHubEntry entry) {
            showProjectHub(context, entry);
        };
    }
    return std::make_unique<EditorSession>(
        m_settingsDirectory, m_project.assetRoot, m_project.sceneRoot, std::move(renderSettings),
        startupSceneLoader(m_project, m_projectHub.has_value()), m_project.gameModule, std::move(services));
}

std::unique_ptr<ApplicationScene> EditorApplication::createProjectHub() {
    ProjectHubConfig config = m_projectHub->hub;
    config.editorSettingsFile = m_settingsDirectory / "settings.ini";
    const ProjectHubEntry entry = std::exchange(m_hubEntry, ProjectHubEntry::Hub);
    return std::make_unique<ProjectHubScene>(*this, m_recentProjects, std::move(config), entry);
}
