#pragma once

#include "engine/module/SystemScheduler.hpp"

#include <memory>
#include <string>

class ComponentTypeRegistry;

// The demo gameplay statically linked into the game executable, registered
// through a ModuleRegistrar exactly as the editor registers the
// zenith_game_demo module: the built-in component types plus the gameplay's,
// and a scheduler for the gameplay's systems.
struct LinkedGameplay {
    std::shared_ptr<const ComponentTypeRegistry> componentTypes;
    std::shared_ptr<const SystemScheduler> systems;
};

struct LinkedGameplayResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Leaves gameplay unchanged on failure.
LinkedGameplayResult linkDemoGameplay(LinkedGameplay& gameplay);
