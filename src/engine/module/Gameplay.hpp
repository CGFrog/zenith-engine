#pragma once

#include "GameServices.hpp"
#include "SystemScheduler.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/input/InputManager.hpp"

#include <filesystem>
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
    // The game's input actions and default bindings; null means none.
    std::shared_ptr<const InputActionMap> inputActions;

    explicit operator bool() const {
        return componentTypes != nullptr && systems != nullptr;
    }
};

// Plays a Gameplay's Play systems in a game's ApplicationScene, in the
// engine's frame order, with a GameServices it owns from start to stop. The
// scene forwards its ApplicationScene calls:
//
//   update            begins the frame (events), runs Update, then timers
//   fixedUpdate       runs FixedUpdate (once per fixed step)
//   afterPhysicsStep  publishes the step's physics events
//   lateUpdate        runs LateUpdate (after physics interpolation)
//   drawUi            runs Ui into the application's GameUiContext
//
// Entity commands flush after every phase. Without start, systems run with
// null services.
//
// The runner owns a copy of the gameplay's input actions (actionMap), which
// the game may rebind (for example with applyInputProfile). It evaluates
// them once per frame and once per fixed step and hands systems the result
// as SystemContext::actions: each fixed step sees the presses buffered since
// the previous step, so a press reaches exactly one fixed step.
class GameplayRunner {
  public:
    GameplayRunner() = default;
    explicit GameplayRunner(Gameplay gameplay);

    GameplayRunner(const GameplayRunner&) = delete;
    GameplayRunner& operator=(const GameplayRunner&) = delete;

    const Gameplay& gameplay() const;

    // Creates the services for context's scene (prefab references are
    // relative to assetRoot) and publishes SceneLoaded. Restarting replaces
    // the services.
    void start(ApplicationContext& context, std::filesystem::path assetRoot);
    // Destroys the services: pending commands, timers, and events are dropped.
    void stop();
    // Null unless started.
    GameServices* services();
    // The input actions systems receive; the game may change the bindings.
    InputActionMap& actionMap();

    // simulatedSeconds is deltaSeconds on the scene's simulation clock; timers
    // advance by it.
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds, float simulatedSeconds);
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void afterPhysicsStep(ApplicationContext& context);
    // LateUpdate and Ui see the input of the frame's update.
    void lateUpdate(ApplicationContext& context);
    void drawUi(ApplicationContext& context);

  private:
    void run(SystemPhase phase, ApplicationContext& context, const InputState* input, const InputActionState* actions,
             float deltaSeconds, GameUiContext* ui = nullptr);

    Gameplay m_gameplay;
    InputActionMap m_actionMap;
    std::unique_ptr<GameServices> m_services;
    // The last update's input, its actions, and its duration.
    InputState m_frameInput;
    InputActionState m_frameActions;
    float m_frameSeconds{0.0f};
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
