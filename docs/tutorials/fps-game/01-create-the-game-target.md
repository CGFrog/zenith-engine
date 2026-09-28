# 1. Create the game target

Zenith separates reusable engine code from game composition. Create this
layout without copying anything into `src/engine/`:

```text
src/fps/
|- application/FpsApplication.hpp
|- application/FpsApplication.cpp
|- player/FpsController.hpp
|- player/FpsController.cpp
|- scene/TrainingScene.hpp
|- scene/TrainingScene.cpp
|- weapons/HitscanWeapon.hpp
`- weapons/HitscanWeapon.cpp
src/fps_main.cpp
tests/fps/FpsGameplayTests.cpp
```

## Application contract

Start with one typed scene ID. `SceneKey` is deliberately generic; the mapping
belongs to the game:

```cpp
// src/fps/application/FpsApplication.hpp
#pragma once

#include "engine/application/Application.hpp"

enum class FpsSceneId : uint32_t { Training };

constexpr SceneKey fpsSceneKey(FpsSceneId scene) {
    return {static_cast<uint32_t>(scene)};
}

class FpsApplication final : public Application {
  public:
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;
};
```

```cpp
// src/fps/application/FpsApplication.cpp
#include "FpsApplication.hpp"
#include "fps/scene/TrainingScene.hpp"

SceneKey FpsApplication::initialScene() const {
    return fpsSceneKey(FpsSceneId::Training);
}

std::unique_ptr<ApplicationScene> FpsApplication::createScene(SceneKey scene) {
    if (scene == fpsSceneKey(FpsSceneId::Training)) {
        return std::make_unique<TrainingScene>();
    }
    return nullptr;
}
```

The executable remains a small composition root:

```cpp
// src/fps_main.cpp
#include "engine/Engine.hpp"
#include "fps/application/FpsApplication.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    Engine engine;
    if (!engine.init()) {
        std::cerr << "FPS initialization failed: " << SDL_GetError() << '\n';
        engine.shutdown();
        return EXIT_FAILURE;
    }

    FpsApplication application;
    const bool success = engine.run(application);
    engine.shutdown();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

## CMake targets

Add explicit sources beside the existing `zenith_demo` and `zenith` targets:

```cmake
add_library(zenith_fps STATIC
    src/fps/application/FpsApplication.cpp
    src/fps/player/FpsController.cpp
    src/fps/scene/TrainingScene.cpp
    src/fps/weapons/HitscanWeapon.cpp
)
target_link_libraries(zenith_fps PUBLIC zenith_engine)

add_executable(zenith_fps_game src/fps_main.cpp)
target_link_libraries(zenith_fps_game PRIVATE zenith_fps)
```

Apply the same warning flags as the other project targets. Because source lists
are explicit, every new `.cpp` file must appear here.

Build only the new executable while iterating:

```powershell
cmake -S . -B build
cmake --build build --config Debug --target zenith_fps_game --parallel 4
build\Debug\zenith_fps_game.exe
```

At this point `TrainingScene` can return success from `load`, do nothing in its
callbacks, and launch an empty window. The next chapter supplies the level.
