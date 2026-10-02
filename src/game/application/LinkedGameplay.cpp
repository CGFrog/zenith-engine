#include "LinkedGameplay.hpp"

#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "game/gameplay/DemoGameplay.hpp"

LinkedGameplayResult linkDemoGameplay(LinkedGameplay& gameplay) {
    auto componentTypes = std::make_shared<ComponentTypeRegistry>(*builtInComponentTypes());
    auto systems = std::make_shared<SystemRegistry>();
    ModuleRegistrar registrar(*componentTypes, *systems);
    registerDemoGameplay(registrar);
    if (!registrar.firstError().empty()) {
        return {.error = "Demo gameplay registration failed: " + registrar.firstError()};
    }
    gameplay.componentTypes = std::move(componentTypes);
    gameplay.systems = std::make_shared<const SystemScheduler>(std::move(systems));
    return {.success = true};
}
