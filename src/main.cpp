#include "engine/Engine.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ContentManifest.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/Scene.hpp"
#include "game/application/DemoApplication.hpp"
#include "game/application/LinkedGameplay.hpp"
#include "game/scene/CaptureCamera.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
int runApplication(int argc, char** argv) {
    initializeResourceLocator(argv[0]);
    std::optional<ProjectConfig> project;
    bool verifyPackage = false;
    int argument = 1;
    if (argument < argc && std::string_view{argv[argument]} == "--verify-package") {
        verifyPackage = true;
        ++argument;
    }
    std::optional<std::filesystem::path> projectFile;
    std::optional<std::filesystem::path> sceneFile;
    for (; argument < argc; argument += 2) {
        const std::string_view flag{argv[argument]};
        if (argument + 1 >= argc || (flag != "--project" && flag != "--scene")) {
            std::cerr << "Usage: zenith [--verify-package] [--project <project-file>] [--scene <scene-file>]\n";
            return EXIT_FAILURE;
        }
        (flag == "--project" ? projectFile : sceneFile) = std::filesystem::path(argv[argument + 1]);
    }
    if (projectFile) {
        ProjectConfig loaded;
        if (const ProjectConfigResult result = loadProjectConfig(*projectFile, loaded); !result) {
            std::cerr << result.error << '\n';
            return EXIT_FAILURE;
        }
        if (loaded.startupScene.empty() && !sceneFile) {
            std::cerr << "Project '" << loaded.displayName << "' does not configure a startup scene\n";
            return EXIT_FAILURE;
        }
        project = std::move(loaded);
    }
    if (sceneFile) {
        // --scene <absolute .zenith-scene>: the editor's Standalone play
        // target (src/editor/application/PlayTargets.hpp) runs an unsaved
        // scene in place of the startup scene, with the project's assets (or
        // the default project's without --project). Its folder becomes the
        // scene root for the run. A minimal parse that the packaged runtime's
        // argument handling may supersede, keeping this contract.
        std::error_code error;
        const std::filesystem::path scene = std::filesystem::absolute(*sceneFile, error).lexically_normal();
        if (error || !std::filesystem::is_regular_file(scene, error)) {
            std::cerr << "Scene file '" << sceneFile->string() << "' does not exist\n";
            return EXIT_FAILURE;
        }
        if (!project) {
            project = ProjectConfig{.displayName = "Zenith Demo", .assetRoot = assetRoot()};
        }
        project->sceneRoot = scene.parent_path();
        project->startupScene = scene;
    }

    if (verifyPackage) {
        const ContentValidationResult content = validateContentManifest(resourceRoot());
        if (!content) {
            for (const ContentValidationIssue& issue : content.issues) {
                std::cerr << issue.path << ": " << issue.message << '\n';
            }
            return EXIT_FAILURE;
        }
        const std::filesystem::path assets = assetRoot();
        const std::filesystem::path shaders = shaderRoot();
        if (!std::filesystem::is_directory(assets) || !std::filesystem::is_directory(shaders) ||
            !std::filesystem::is_regular_file(shaders / "mesh.vert.spv") ||
            !std::filesystem::is_regular_file(shaders / "post.frag.spv")) {
            std::cerr << "Package resources are incomplete under " << resourceRoot() << '\n';
            return EXIT_FAILURE;
        }
        if (project) {
            LinkedGameplay gameplay;
            if (const LinkedGameplayResult result = linkDemoGameplay(gameplay); !result) {
                std::cerr << result.error << '\n';
                return EXIT_FAILURE;
            }
            AssetManager assetManager;
            Scene scene;
            if (const ProjectConfigResult result =
                    loadProjectStartupScene(*project, assetManager, scene, gameplay.componentTypes);
                !result) {
                std::cerr << result.error << '\n';
                return EXIT_FAILURE;
            }
        }
        std::cout << "Verified Zenith package resources at " << resourceRoot() << '\n';
        return EXIT_SUCCESS;
    }

    const CaptureCameraResult captureCamera = captureCameraFromEnvironment();
    if (!captureCamera) {
        std::cerr << captureCamera.error << '\n';
        return EXIT_FAILURE;
    }

    Engine engine;

    if (!engine.init()) {
        std::cerr << "Zenith initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    // Cooked textures produced by zenith_asset_build live beside the packaged assets.
    engine.assets().setCookedTextureRoot(resourceRoot() / "derived");

    DemoApplication application(std::move(project), userDataDirectory("Zenith", "Demo") / "game-settings.json",
                                captureCamera.path);
    const bool success = engine.run(application);
    engine.shutdown();
    return validationExitCode(engine.graphicsValidation(), success ? EXIT_SUCCESS : EXIT_FAILURE);
}
} // namespace

int main(int argc, char** argv) {
    try {
        return runApplication(argc, argv);
    } catch (const std::exception& error) {
        std::filesystem::path reportPath;
        DiagnosticBundleWriter diagnostics(userDataDirectory("Zenith", "Demo") / "diagnostics");
        diagnostics.write({"latest", "0.1.0", error.what(), {}}, {}, reportPath);
        std::cerr << "Zenith stopped after an unexpected error: " << error.what() << "\nDiagnostic: " << reportPath
                  << '\n';
        return EXIT_FAILURE;
    }
}
