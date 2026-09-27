#include "editor/application/EditorApplication.hpp"
#include "editor/project/ProjectConfig.hpp"
#include "engine/Engine.hpp"
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

    ProjectConfig project{.assetRoot = ZENITH_ASSET_DIR, .sceneRoot = std::filesystem::current_path() / "scenes"};
    if (argc > 1) {
        if (const ProjectConfigResult result = loadProjectConfig(argv[1], project); !result) {
            std::cerr << result.error << '\n';
            engine.shutdown();
            return EXIT_FAILURE;
        }
    }
    EditorApplication application(
        std::filesystem::current_path() / ".zenith" / "editor", std::move(project),
        [](ApplicationContext& context) { return populateDemoScene(context.scene, context.assets); });
    const bool success = engine.run(application);
    engine.shutdown();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
