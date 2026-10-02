#include "editor/application/EditorApplication.hpp"
#include "engine/Engine.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>

int main(int argc, char** argv) {
    initializeResourceLocator(argv[0]);
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Zenith Editor initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }
    // Cooked textures produced by zenith_asset_build live beside the packaged assets.
    engine.assets().setCookedTextureRoot(resourceRoot() / "derived");

    // Image-based lighting draws the environment's procedural sky behind the
    // default scene; capability checks fall back to hemisphere lighting.
    ProjectConfig project{.displayName = "Zenith Demo",
                          .assetRoot = assetRoot(),
                          .sceneRoot = std::filesystem::current_path() / "scenes",
                          .renderSettings = RenderSettings{.environment = EnvironmentQuality::ImageBasedLow}};
    bool projectLoaded = false;
    if (argc > 1) {
        if (const ProjectConfigResult result = loadProjectConfig(argv[1], project); !result) {
            std::cerr << result.error << '\n';
            engine.shutdown();
            return EXIT_FAILURE;
        }
        projectLoaded = true;
    }
    // Without a startup scene the editor opens its default scene.
    std::function<SceneResult(ApplicationContext&)> initialSceneLoader;
    if (projectLoaded && !project.startupScene.empty()) {
        initialSceneLoader = [project](ApplicationContext& context) -> SceneResult {
            const ProjectConfigResult result = loadProjectStartupScene(project, context.assets, context.scene);
            return SceneResult{.success = result.success, .error = result.error};
        };
    }
    EditorApplication application(userDataDirectory("Zenith", "Editor"),
                                  resourceRoot() / "editor" / "fonts" / "Inter-Regular.ttf", std::move(project),
                                  std::move(initialSceneLoader));
    const bool success = engine.run(application);
    engine.shutdown();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
