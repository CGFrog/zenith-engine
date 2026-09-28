#include "engine/Engine.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "game/application/DemoApplication.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

int main(int argc, char** argv) {
    std::optional<ProjectConfig> project;
    if (argc != 1) {
        if (argc != 3 || std::string_view{argv[1]} != "--project") {
            std::cerr << "Usage: zenith [--project <project-file>]\n";
            return EXIT_FAILURE;
        }
        ProjectConfig loaded;
        if (const ProjectConfigResult result = loadProjectConfig(argv[2], loaded); !result) {
            std::cerr << result.error << '\n';
            return EXIT_FAILURE;
        }
        if (loaded.startupScene.empty()) {
            std::cerr << "Project '" << loaded.displayName << "' does not configure a startup scene\n";
            return EXIT_FAILURE;
        }
        project = std::move(loaded);
    }

    Engine engine;

    if (!engine.init()) {
        std::cerr << "Zenith initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    DemoApplication application(std::move(project), std::filesystem::current_path() / ".zenith" / "game-settings.json");
    const bool success = engine.run(application);
    engine.shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
