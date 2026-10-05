#pragma once

#include "GameServices.hpp"
#include "SystemScheduler.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/input/InputManager.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class BehaviourRegistry;
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
    // The game's behaviour types; null means none.
    std::shared_ptr<const BehaviourRegistry> behaviours;

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
//
// A system that throws is logged and disabled until the next start or stop
// (systemFaults), and the game plays on without it.
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
    // Runs onDisable and onDestroy for the behaviours of context's scene, then
    // destroys the services: pending commands, timers, and events are dropped.
    // Call it before the scene is discarded (ApplicationScene::unload).
    void stop(ApplicationContext& context);
    // Null unless started.
    GameServices* services();
    // The systems that threw since the last start or stop, which no longer run.
    const SystemFaults& systemFaults() const;
    // The input actions systems receive; the game may change the bindings.
    InputActionMap& actionMap();
    // The game's clock (GameServices::simulationClock), which the scene
    // reports as its ApplicationScene::simulationClock so the engine steps
    // physics, animation, and audio by it; the default clock unless started.
    SimulationClock simulationClock() const;

    // deltaSeconds is the frame's wall-clock time. Update and LateUpdate
    // systems and the timers get it on simulationClock(); Ui systems and
    // SystemContext::unscaledDeltaSeconds get it unchanged.
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void afterPhysicsStep(ApplicationContext& context);
    // LateUpdate and Ui see the input of the frame's update.
    void lateUpdate(ApplicationContext& context);
    void drawUi(ApplicationContext& context);

  private:
    void run(SystemPhase phase, ApplicationContext& context, const InputState* input, const InputActionState* actions,
             float deltaSeconds, float unscaledDeltaSeconds, GameUiContext* ui = nullptr);

    Gameplay m_gameplay;
    InputActionMap m_actionMap;
    std::unique_ptr<GameServices> m_services;
    SystemFaults m_systemFaults;
    // The last update's input, its actions, and its wall-clock duration.
    InputState m_frameInput;
    InputActionState m_frameActions;
    float m_unscaledFrameSeconds{0.0f};
};

struct GameplayResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The built-in component types and the engine's systems (builtInSystems).
Gameplay builtInGameplay();
// The module's component types and systems; the module must outlive them.
Gameplay gameplayFromModule(const GameModule& module);
// Runs registration (a module's zenith_module_register linked into the
// executable) through a ModuleRegistrar over the built-in component types
// and systems.
// Leaves gameplay unchanged on failure.
GameplayResult linkGameplay(const std::function<void(ModuleRegistrar&)>& registration, Gameplay& gameplay);
