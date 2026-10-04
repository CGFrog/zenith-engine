#include "engine/Engine.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "game/application/DemoApplication.hpp"
#include "game/application/GameLaunch.hpp"
#include "game/application/PackageVerification.hpp"
#include "game/application/ProjectApplication.hpp"
#include "game/application/RuntimeGameplay.hpp"
#include "game/scene/CaptureCamera.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {
int runApplication(int argc, char** argv) {
    const std::vector<std::string> commandLine = utf8CommandLine(argc, argv);
    initializeResourceLocator(commandLine.empty() ? std::filesystem::path(argv[0]) : pathFromUtf8(commandLine[0]));
    const std::vector<std::string_view> arguments(commandLine.begin() + (commandLine.empty() ? 0 : 1),
                                                  commandLine.end());
    GameLaunchOptions launch;
    if (const GameLaunchResult result = parseGameLaunchArguments(arguments, launch); !result) {
        std::cerr << result.error << '\n' << gameUsage << '\n';
        return EXIT_FAILURE;
    }
    std::optional<ProjectConfig> project;
    if (const GameLaunchResult result = loadLaunchProject(launch, resourceRoot(), project); !result) {
        std::cerr << result.error << '\n';
        return EXIT_FAILURE;
    }

    // Declared before the engine and the application: the game module's code
    // must outlive every scene its gameplay touched, the engine's included.
    RuntimeGameplay runtime;
    const GameModuleLoading loading = project && !project->gameModule.empty()
                                          ? runtimeModuleLoading(project->gameModule, resourceRoot())
                                          : GameModuleLoading::ShadowCopy;
    if (const GameplayResult result = loadRuntimeGameplay(project ? &*project : nullptr, loading, runtime); !result) {
        std::cerr << result.error << '\n';
        return EXIT_FAILURE;
    }
    if (runtime.module) {
        std::cout << "Loaded game module '" << runtime.module->name() << "' " << runtime.module->version() << " from "
                  << utf8FromPath(runtime.module->loadedPath()) << '\n';
    }

    if (launch.verifyPackage) {
        const PackageVerificationResult verified =
            verifyPackage(resourceRoot(), project ? &*project : nullptr, runtime.gameplay);
        if (!verified) {
            std::cerr << verified.error << '\n';
            return EXIT_FAILURE;
        }
        for (const std::filesystem::path& scene : verified.scenes) {
            std::cout << "Verified scene " << utf8FromPath(scene) << '\n';
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

    std::unique_ptr<Application> application;
    if (runtime.presentation == GamePresentation::Demo) {
        application = std::make_unique<DemoApplication>(std::move(project),
                                                        userDataDirectory("Zenith", "Demo") / "game-settings.json",
                                                        captureCamera.path, runtime.gameplay);
    } else {
        application = std::make_unique<ProjectApplication>(std::move(*project), runtime.gameplay, captureCamera.path);
    }
    const bool success = engine.run(*application);
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
