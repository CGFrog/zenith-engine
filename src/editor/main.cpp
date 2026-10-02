#include "editor/application/EditorApplication.hpp"
#include "editor/project/IrradianceVolumeBaker.hpp"
#include "engine/Engine.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string_view>

namespace {
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
