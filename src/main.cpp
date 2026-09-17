#include "engine/Engine.hpp"
#include "engine/scene/TemplateScene.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    Engine engine;

    if (!engine.init()) {
        std::cerr << "Zenith initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    if (const SceneResult result = createTemplateScene(engine); !result) {
        std::cerr << result.error << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    const bool success = engine.run();
    engine.shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
