#include "engine/Engine.hpp"
#include "game/application/DemoApplication.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    Engine engine;

    if (!engine.init()) {
        std::cerr << "Zenith initialization failed. SDL: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    DemoApplication application;
    const bool success = engine.run(application);
    engine.shutdown();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
