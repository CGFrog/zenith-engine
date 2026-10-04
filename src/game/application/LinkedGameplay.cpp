#include "LinkedGameplay.hpp"

#include "game/gameplay/DemoGameplay.hpp"

GameplayResult linkDemoGameplay(Gameplay& gameplay) {
    if (GameplayResult result = linkGameplay(registerDemoGameplay, gameplay); !result) {
        return {.error = "Demo gameplay registration failed: " + result.error};
    }
    return {.success = true};
}
