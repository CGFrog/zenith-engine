#include "Gameplay.hpp"

#include "BuiltInSystems.hpp"
#include "GameEvents.hpp"
#include "GameModule.hpp"
#include "ModuleRegistrar.hpp"
#include "engine/application/Application.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
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
    m_services->events().publish(SceneLoaded{});
}

void GameplayRunner::stop() {
    m_services.reset();
}

GameServices* GameplayRunner::services() {
    return m_services.get();
}

InputActionMap& GameplayRunner::actionMap() {
    return m_actionMap;
}

void GameplayRunner::update(ApplicationContext& context, const InputState& input, float deltaSeconds,
                            float simulatedSeconds) {
    m_frameInput = input;
    m_frameActions = m_actionMap.evaluate(input);
    m_frameSeconds = deltaSeconds;
    if (m_services) {
        m_services->beginFrame();
    }
    run(SystemPhase::Update, context, &m_frameInput, &m_frameActions, deltaSeconds);
    if (m_services) {
        m_services->advanceTimers(context.scene, simulatedSeconds);
    }
}

void GameplayRunner::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    const InputActionState actions = m_actionMap.evaluate(input);
    run(SystemPhase::FixedUpdate, context, &input, &actions, deltaSeconds);
}

void GameplayRunner::afterPhysicsStep(ApplicationContext& context) {
    if (m_services) {
        m_services->publishPhysicsEvents(context.scene, context.physics.triggerEvents(),
                                         context.physics.contactEvents());
    }
}

void GameplayRunner::lateUpdate(ApplicationContext& context) {
    run(SystemPhase::LateUpdate, context, &m_frameInput, &m_frameActions, m_frameSeconds);
}

void GameplayRunner::drawUi(ApplicationContext& context) {
    run(SystemPhase::Ui, context, &m_frameInput, &m_frameActions, m_frameSeconds, &context.gameUi);
}

void GameplayRunner::run(SystemPhase phase, ApplicationContext& context, const InputState* input,
                         const InputActionState* actions, float deltaSeconds, GameUiContext* ui) {
    if (!m_gameplay.systems) {
        return;
    }
    runGameplayPhase(*m_gameplay.systems, phase,
                     {.scene = context.scene,
                      .application = &context,
                      .mode = SimulationMode::Play,
                      .deltaSeconds = deltaSeconds,
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
