#include "engine/Engine.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ContentManifest.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/Scene.hpp"
#include "game/application/DemoApplication.hpp"

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
    if (argument < argc) {
        if (argument + 2 != argc || std::string_view{argv[argument]} != "--project") {
            std::cerr << "Usage: zenith [--verify-package] [--project <project-file>]\n";
            return EXIT_FAILURE;
        }
        ProjectConfig loaded;
        if (const ProjectConfigResult result = loadProjectConfig(argv[argument + 1], loaded); !result) {
            std::cerr << result.error << '\n';
            return EXIT_FAILURE;
        }
        if (loaded.startupScene.empty()) {
            std::cerr << "Project '" << loaded.displayName << "' does not configure a startup scene\n";
            return EXIT_FAILURE;
        }
        project = std::move(loaded);
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
            AssetManager assetManager;
            Scene scene;
            if (const ProjectConfigResult result = loadProjectStartupScene(*project, assetManager, scene); !result) {
                std::cerr << result.error << '\n';
                return EXIT_FAILURE;
            }
        }
        std::cout << "Verified Zenith package resources at " << resourceRoot() << '\n';
        return EXIT_SUCCESS;
    }

    Engine engine;

    if (!engine.init()) {
        std::cerr << "Zenith initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }
    // Cooked textures produced by zenith_asset_build live beside the packaged assets.
    engine.assets().setCookedTextureRoot(resourceRoot() / "derived");

    DemoApplication application(std::move(project), userDataDirectory("Zenith", "Demo") / "game-settings.json");
    const bool success = engine.run(application);
    engine.shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
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
