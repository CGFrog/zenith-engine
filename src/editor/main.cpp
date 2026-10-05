#include "editor/application/EditorApplication.hpp"
#include "editor/project/IrradianceVolumeBaker.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/Engine.hpp"
#include "engine/platform/CrashHandler.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// The Project Hub's template, engine build, and default folder. Templates
// are looked up beside the executable's resources first (<resource
// root>/templates, where the build copies them), then in the source tree's
// templates/. Generated projects build against the resource root when it is
// a Zenith build tree (it holds ZenithEngineConfig.cmake), else against the
// build this editor was compiled in.
std::array<std::filesystem::path, 2> templateRoots() {
    return {resourceRoot() / "templates", fromUtf8(ZENITH_SOURCE_TEMPLATE_DIR)};
}

// The C++ game template's snippets, which the module.new_* code generators
// write new types from; empty when no template is found.
std::filesystem::path codeSnippetDirectory() {
    const std::filesystem::path cppGame = findProjectTemplate(templateRoots(), cppGameTemplateName);
    return cppGame.empty() ? cppGame : cppGame / templateSnippetDirectoryName;
}

EditorProjectHubOptions projectHubOptions(bool startInHub) {
    const std::array engineRoots{resourceRoot(), fromUtf8(ZENITH_ENGINE_BUILD_DIR)};
    std::filesystem::path engineRoot = findEngineBuildRoot(engineRoots);
    if (engineRoot.empty()) {
        engineRoot = fromUtf8(ZENITH_ENGINE_BUILD_DIR);
    }
    // SDL owns the returned string.
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    std::filesystem::path projectsFolder =
        (documents != nullptr ? fromUtf8(documents) : std::filesystem::current_path()) / "Zenith Projects";
    return {.hub = {.templates = listProjectTemplates(templateRoots()),
                    .engineRoot = std::move(engineRoot),
                    .engineAssetRoot = assetRoot(),
                    .buildConfiguration = ZENITH_EDITOR_CONFIGURATION,
                    .defaultProjectLocation = std::move(projectsFolder)},
            .startInHub = startInHub};
}

// `zenith_editor <project> --bake-irradiance-volumes` bakes every irradiance
// volume in the project's startup scene and saves the scene, without a window.
int bakeIrradianceVolumes(const ProjectConfig& project) {
    AssetManager assets;
    Scene scene;
    if (const ProjectConfigResult loaded = loadProjectStartupScene(project, assets, scene); !loaded) {
        std::cerr << loaded.error << '\n';
        return EXIT_FAILURE;
    }
    uint32_t volumesBaked = 0;
    if (const IrradianceVolumeBakeResult baked =
            bakeSceneIrradianceVolumes(scene, assets, project.assetRoot, project.startupScene, {}, volumesBaked);
        !baked) {
        std::cerr << "Irradiance volume bake failed: " << baked.error << '\n';
        return EXIT_FAILURE;
    }
    if (const SceneDocumentResult saved =
            SceneDocument::save(project.startupScene, scene, assets, {.assetRoot = project.assetRoot});
        !saved) {
        std::cerr << saved.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Baked " << volumesBaked << " irradiance volume(s) into " << project.startupScene.string() << '\n';
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv) {
    initializeResourceLocator(argv[0]);
    // A crash leaves a minidump and a report in the editor's diagnostics
    // folder; the open session adds an emergency save of its scene to the
    // recovery file (EditorApplication::setCrashHandler).
    CrashHandlerScope crashHandler({.directory = userDataDirectory("Zenith", "Editor") / "diagnostics",
                                    .product = "zenith_editor",
                                    .productVersion = ZENITH_EDITOR_CONFIGURATION});
    if (argc > 2 && std::string_view(argv[2]) == "--bake-irradiance-volumes") {
        ProjectConfig project;
        if (const ProjectConfigResult result = loadProjectConfig(argv[1], project); !result) {
            std::cerr << result.error << '\n';
            return EXIT_FAILURE;
        }
        return bakeIrradianceVolumes(project);
    }
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Zenith Editor initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    // Cooked textures produced by zenith_asset_build live beside the packaged assets.
    engine.assets().setCookedTextureRoot(resourceRoot() / "derived");

    // Image-based lighting draws the environment's procedural sky behind the
    // default scene; capability checks fall back to hemisphere lighting.
    ProjectConfig project{.displayName = "Zenith Demo",
                          .assetRoot = assetRoot(),
                          .sceneRoot = std::filesystem::current_path() / "scenes",
                          .renderSettings = RenderSettings{.environment = EnvironmentQuality::ImageBasedLow}};
    // Without a startup scene the editor opens its default scene.
    if (argc > 1) {
        if (const ProjectConfigResult result = loadProjectConfig(argv[1], project); !result) {
            std::cerr << result.error << '\n';
            engine.shutdown();
            return EXIT_FAILURE;
        }
    }
    // Interactive runs offer the Project Hub and start in it without a
    // project argument. Smoke runs never show it: without a project they
    // open the default project, as before the hub existed.
    std::optional<EditorProjectHubOptions> projectHub;
    if (SDL_getenv("ZENITH_EDITOR_SMOKE") == nullptr) {
        projectHub = projectHubOptions(argc <= 1);
    }
    // Automated runs pass their own settings directory so they never read or
    // replace the user's workspaces and preferences.
    const char* settingsOverride = SDL_getenv(editorSettingsDirectoryVariable);
    // Play's Standalone target runs the game built (or packaged) beside the
    // editor.
    const char* editorDirectory = SDL_GetBasePath();
    EditorPlayServices play{.gameExecutable =
                                gameExecutableBeside(editorDirectory != nullptr ? fromUtf8(editorDirectory)
                                                                                : std::filesystem::current_path())};
    EditorApplication application(resolveEditorSettingsDirectory(settingsOverride != nullptr ? settingsOverride : "",
                                                                 userDataDirectory("Zenith", "Editor")),
                                  resourceRoot() / "editor" / "fonts" / "Inter-Regular.ttf", std::move(project),
                                  std::move(projectHub), std::move(play), codeSnippetDirectory());
    application.setCrashHandler(crashHandler);
    const bool success = engine.run(application);
    engine.shutdown();
    return validationExitCode(engine.graphicsValidation(), success ? EXIT_SUCCESS : EXIT_FAILURE);
}
