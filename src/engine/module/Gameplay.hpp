#pragma once

#include "SystemScheduler.hpp"

#include <functional>
#include <memory>
#include <string>

class ComponentTypeRegistry;
class GameModule;
class ModuleRegistrar;

// What a game plays: the component types its scenes load with and the systems
// it runs while playing. It comes from a game module (gameplayFromModule),
// from gameplay linked into the executable (linkGameplay), or is the engine's
// built-in component types alone (builtInGameplay).
//
// Lifetime: gameplay from a module holds the module's code, so every copy, and
// every scene its systems or component types touched, must be destroyed
// before the module (see GameModule).
struct Gameplay {
    std::shared_ptr<const ComponentTypeRegistry> componentTypes;
    std::shared_ptr<const SystemScheduler> systems;

    explicit operator bool() const {
        return componentTypes != nullptr && systems != nullptr;
    }

    // Runs the systems of phase that run in Play over context.scene, with the
    // application's services and the frame's input. deltaSeconds is the frame
    // duration for Update and the fixed step for FixedUpdate. Does nothing
    // without systems.
    void play(SystemPhase phase, ApplicationContext& context, const InputState& input, float deltaSeconds) const;
};

struct GameplayResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The built-in component types and no systems.
Gameplay builtInGameplay();
// The module's component types and systems; the module must outlive them.
Gameplay gameplayFromModule(const GameModule& module);
// Runs registration (a module's zenith_module_register linked into the
// executable) through a ModuleRegistrar over the built-in component types.
// Leaves gameplay unchanged on failure.
GameplayResult linkGameplay(const std::function<void(ModuleRegistrar&)>& registration, Gameplay& gameplay);
