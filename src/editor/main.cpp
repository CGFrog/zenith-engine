#include "editor/application/EditorApplication.hpp"
#include "engine/Engine.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/scene/DemoScene.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Zenith Editor initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    ProjectConfig project{.displayName = "Zenith Demo",
                          .assetRoot = ZENITH_ASSET_DIR,
                          .sceneRoot = std::filesystem::current_path() / "scenes"};
    bool projectLoaded = false;
    if (argc > 1) {
        if (const ProjectConfigResult result = loadProjectConfig(argv[1], project); !result) {
            std::cerr << result.error << '\n';
            engine.shutdown();
            return EXIT_FAILURE;
        }
        projectLoaded = true;
    }
    auto initialSceneLoader =
        projectLoaded
            ? std::function<SceneResult(ApplicationContext&)>{[project](ApplicationContext& context) -> SceneResult {
                  if (project.startupScene.empty()) {
                      return {.success = true};
                  }
                  const ProjectConfigResult result = loadProjectStartupScene(project, context.assets, context.scene);
                  return SceneResult{.success = result.success, .error = result.error};
              }}
            : std::function<SceneResult(ApplicationContext&)>{
                  [](ApplicationContext& context) { return populateDemoScene(context.scene, context.assets); }};
    EditorApplication application(std::filesystem::current_path() / ".zenith" / "editor", std::move(project),
                                  std::move(initialSceneLoader));
    const bool success = engine.run(application);
    engine.shutdown();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
