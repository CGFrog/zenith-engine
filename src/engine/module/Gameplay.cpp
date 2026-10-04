#include "Gameplay.hpp"

#include "BuiltInSystems.hpp"
#include "GameEvents.hpp"
#include "GameModule.hpp"
#include "ModuleRegistrar.hpp"
#include "engine/application/Application.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
#include "engine/behaviour/BehaviourScheduler.hpp"
#include "engine/scene/ComponentTypes.hpp"

GameplayRunner::GameplayRunner(Gameplay gameplay)
    : m_gameplay(std::move(gameplay))
    , m_actionMap(m_gameplay.inputActions ? *m_gameplay.inputActions : InputActionMap{}) {}

const Gameplay& GameplayRunner::gameplay() const {
    return m_gameplay;
}

void GameplayRunner::start(ApplicationContext& context, std::filesystem::path assetRoot) {
    m_services.reset();
    m_services = std::make_unique<GameServices>(
        context.assets,
        SceneDocumentOptions{.assetRoot = std::move(assetRoot), .componentTypes = m_gameplay.componentTypes},
        context.rendering.debugDraw(), &context.logger, m_gameplay.behaviours);
    m_services->setAudio(&context.audio);
    m_services->events().publish(SceneLoaded{});
}

void GameplayRunner::stop(ApplicationContext& context) {
    if (m_services && m_gameplay.behaviours) {
        destroyAllBehaviours(*m_gameplay.behaviours, context.scene, m_services.get(), &context.logger);
    }
    m_services.reset();
}

GameServices* GameplayRunner::services() {
    return m_services.get();
}

InputActionMap& GameplayRunner::actionMap() {
    return m_actionMap;
}

SimulationClock GameplayRunner::simulationClock() const {
    return m_services ? m_services->simulationClock() : SimulationClock{};
}

void GameplayRunner::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    m_frameInput = input;
    m_frameActions = m_actionMap.evaluate(input);
    m_unscaledFrameSeconds = deltaSeconds;
    const float seconds = simulatedSeconds(simulationClock(), deltaSeconds);
    if (m_services) {
        m_services->beginFrame();
    }
    run(SystemPhase::Update, context, &m_frameInput, &m_frameActions, seconds, deltaSeconds);
    if (m_services) {
        // On the clock as the systems left it, so a pause they requested
        // holds the timers this frame.
        m_services->advanceTimers(context.scene, simulatedSeconds(simulationClock(), deltaSeconds));
    }
}

void GameplayRunner::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    const InputActionState actions = m_actionMap.evaluate(input);
    run(SystemPhase::FixedUpdate, context, &input, &actions, deltaSeconds, deltaSeconds);
}

void GameplayRunner::afterPhysicsStep(ApplicationContext& context) {
    if (m_services) {
        m_services->publishPhysicsEvents(context.scene, context.physics.triggerEvents(),
                                         context.physics.contactEvents());
    }
}

void GameplayRunner::lateUpdate(ApplicationContext& context) {
    // The engine stepped this frame by the clock as Update left it.
    run(SystemPhase::LateUpdate, context, &m_frameInput, &m_frameActions,
        simulatedSeconds(simulationClock(), m_unscaledFrameSeconds), m_unscaledFrameSeconds);
}

void GameplayRunner::drawUi(ApplicationContext& context) {
    run(SystemPhase::Ui, context, &m_frameInput, &m_frameActions, m_unscaledFrameSeconds, m_unscaledFrameSeconds,
        &context.gameUi);
}

void GameplayRunner::run(SystemPhase phase, ApplicationContext& context, const InputState* input,
                         const InputActionState* actions, float deltaSeconds, float unscaledDeltaSeconds,
                         GameUiContext* ui) {
    if (!m_gameplay.systems) {
        return;
    }
    runGameplayPhase(*m_gameplay.systems, phase,
                     {.scene = context.scene,
                      .application = &context,
                      .mode = SimulationMode::Play,
                      .deltaSeconds = deltaSeconds,
                      .unscaledDeltaSeconds = unscaledDeltaSeconds,
                      .input = input,
                      .logger = &context.logger,
                      .services = m_services.get(),
                      .actions = actions,
                      .ui = ui,
                      .behaviours = m_gameplay.behaviours.get()});
}

Gameplay builtInGameplay() {
    return {.componentTypes = builtInComponentTypes(),
            .systems = std::make_shared<const SystemScheduler>(builtInSystems()),
            .inputActions = std::make_shared<const InputActionMap>(),
            .behaviours = std::make_shared<const BehaviourRegistry>()};
}

Gameplay gameplayFromModule(const GameModule& module) {
    return {.componentTypes = module.componentTypes(),
            .systems = std::make_shared<const SystemScheduler>(module.systems()),
            .inputActions = module.inputActions(),
            .behaviours = module.behaviours()};
}

GameplayResult linkGameplay(const std::function<void(ModuleRegistrar&)>& registration, Gameplay& gameplay) {
    auto componentTypes = std::make_shared<ComponentTypeRegistry>(*builtInComponentTypes());
    auto systems = std::make_shared<SystemRegistry>(*builtInSystems());
    auto inputActions = std::make_shared<InputActionMap>();
    auto behaviours = std::make_shared<BehaviourRegistry>();
    ModuleRegistrar registrar(*componentTypes, *systems, *inputActions, *behaviours);
    registration(registrar);
    if (!registrar.firstError().empty()) {
        return {.error = registrar.firstError()};
    }
    gameplay.componentTypes = std::move(componentTypes);
    gameplay.systems = std::make_shared<const SystemScheduler>(std::move(systems));
    gameplay.inputActions = std::move(inputActions);
    gameplay.behaviours = std::move(behaviours);
    return {.success = true};
}
