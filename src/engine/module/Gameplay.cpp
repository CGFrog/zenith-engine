#include "Gameplay.hpp"

#include "GameModule.hpp"
#include "ModuleRegistrar.hpp"
#include "engine/application/Application.hpp"
#include "engine/scene/ComponentTypes.hpp"

void Gameplay::play(SystemPhase phase, ApplicationContext& context, const InputState& input, float deltaSeconds) const {
    if (!systems) {
        return;
    }
    systems->run(phase, {.scene = context.scene,
                         .application = &context,
                         .mode = SimulationMode::Play,
                         .deltaSeconds = deltaSeconds,
                         .input = &input});
}

Gameplay builtInGameplay() {
    return {.componentTypes = builtInComponentTypes(),
            .systems = std::make_shared<const SystemScheduler>(std::make_shared<const SystemRegistry>())};
}

Gameplay gameplayFromModule(const GameModule& module) {
    return {.componentTypes = module.componentTypes(),
            .systems = std::make_shared<const SystemScheduler>(module.systems())};
}

GameplayResult linkGameplay(const std::function<void(ModuleRegistrar&)>& registration, Gameplay& gameplay) {
    auto componentTypes = std::make_shared<ComponentTypeRegistry>(*builtInComponentTypes());
    auto systems = std::make_shared<SystemRegistry>();
    ModuleRegistrar registrar(*componentTypes, *systems);
    registration(registrar);
    if (!registrar.firstError().empty()) {
        return {.error = registrar.firstError()};
    }
    gameplay.componentTypes = std::move(componentTypes);
    gameplay.systems = std::make_shared<const SystemScheduler>(std::move(systems));
    return {.success = true};
}
