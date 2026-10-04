#include "EditorApplication.hpp"

#include "EditorSession.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/theme/ThemeChrome.hpp"

#include <imgui.h>

#include <SDL3/SDL_video.h>

#include <chrono>
#include <utility>

namespace {
constexpr SceneKey editorSessionKey{0};
constexpr SceneKey projectHubKey{1};
constexpr const char* logCategory = "Editor";

void logAppearanceResult(const EditorAppearanceResult& result, Logger& logger) {
    if (!result) {
        logger.log(LogLevel::Error, logCategory, result.error);
    }
    for (const std::string& warning : result.warnings) {
        logger.log(LogLevel::Warning, logCategory, warning);
    }
}

// The pixel theme on the application's ImGui context, in the look settings.ini
// chooses (its UI scale and theme tuning; the defaults when the file is
// missing or invalid), plus popup bevels and no imgui.ini (each workspace
// keeps its own layout in workspaces.json). Fonts must be added before the
// first frame builds the atlas, so this runs once, before the hub or the
// first session draws.
std::unique_ptr<EditorAppearance> installEditorTheme(const std::filesystem::path& fontPath,
                                                     const std::filesystem::path& settingsFile, Logger& logger) {
    // The automatic UI scale follows the display's scale factor so text stays
    // crisp on high-DPI monitors; the pixel theme rounds it to a whole number
    // of screen pixels per art pixel.
    auto appearance = std::make_unique<EditorAppearance>(fontPath, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    EditorSettings settings(settingsFile);
    if (const EditorSettingsResult loaded = settings.load(); !loaded) {
        logger.log(LogLevel::Warning, logCategory, loaded.error);
    }
    logAppearanceResult(appearance->apply(settings.appearance()), logger);
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    ImGui::GetIO().IniFilename = nullptr;
    return appearance;
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

// The hub as an application scene: no world, only the hub over the wallpaper,
// in the look of the application's appearance (the last session's).
class ProjectHubScene final : public ApplicationScene {
  public:
    ProjectHubScene(EditorApplication& application, const EditorAppearance& appearance, RecentProjects& recentProjects,
                    ProjectHubConfig config, ProjectHubEntry entry)
        : m_application(application)
        , m_appearance(appearance)
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
        m_chrome.setParameters(m_appearance.theme());
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
    const EditorAppearance& m_appearance;
    ProjectHub m_hub;
    ProjectHubEntry m_entry;
    ThemeChrome m_chrome;
};
} // namespace

EditorApplication::EditorApplication(std::filesystem::path settingsDirectory, std::filesystem::path fontPath,
                                     ProjectConfig project, std::optional<EditorProjectHubOptions> projectHub,
                                     EditorPlayServices play, std::filesystem::path codeSnippetDirectory)
    : m_settingsDirectory(std::move(settingsDirectory))
    , m_fontPath(std::move(fontPath))
    , m_project(std::move(project))
    , m_projectHub(std::move(projectHub))
    , m_play(std::move(play))
    , m_codeSnippetDirectory(std::move(codeSnippetDirectory))
    , m_recentProjects(m_settingsDirectory / "recent-projects.json") {}

SceneResult EditorApplication::onStart(ApplicationContext& context) {
    m_appearance = installEditorTheme(m_fontPath, m_settingsDirectory / "settings.ini", context.logger);
    if (const RecentProjectsResult loaded = m_recentProjects.load(); !loaded) {
        context.logger.log(LogLevel::Warning, logCategory, loaded.error);
    }
    if (initialScene() == editorSessionKey) {
        recordRecentProject(context.logger);
    }
    return {.success = true};
}

MouseCapturePolicy EditorApplication::mouseCapturePolicy() const {
    return m_mouseCapturePolicy;
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
    EditorProjectServices services{
        .build = projectBuildRequest(m_project.projectFile),
        .projectFile = m_project.projectFile,
        .codeSnippetDirectory = m_codeSnippetDirectory,
        .physicsLayers = m_project.physicsLayerTable(),
        .play = m_play,
        // The session publishes its policy every frame and resets it
        // when it unloads, so a session that failed to load or was
        // replaced never leaves the cursor captured.
        .mouseCaptureChanged = [this](MouseCapturePolicy policy) { m_mouseCapturePolicy = policy; }};
    if (m_projectHub) {
        services.leaveForProjectHub = [this](ApplicationContext& context, ProjectHubEntry entry) {
            showProjectHub(context, entry);
        };
    }
    return std::make_unique<EditorSession>(
        m_settingsDirectory, m_project.assetRoot, m_project.sceneRoot, std::move(renderSettings), *m_appearance,
        startupSceneLoader(m_project, m_projectHub.has_value()), m_project.gameModule, std::move(services));
}

std::unique_ptr<ApplicationScene> EditorApplication::createProjectHub() {
    ProjectHubConfig config = m_projectHub->hub;
    config.editorSettingsFile = m_settingsDirectory / "settings.ini";
    const ProjectHubEntry entry = std::exchange(m_hubEntry, ProjectHubEntry::Hub);
    return std::make_unique<ProjectHubScene>(*this, *m_appearance, m_recentProjects, std::move(config), entry);
}
