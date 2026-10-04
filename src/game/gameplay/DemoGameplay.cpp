#include "DemoGameplay.hpp"

#include "engine/application/Application.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "game/gameplay/ItemCollection.hpp"
#include "game/input/DemoInput.hpp"

#include <memory>
#include <string>

namespace {
SystemDescriptor itemCollectionSystem() {
    return {.name = std::string(DemoGameplay::ItemCollectionSystem),
            .phase = SystemPhase::FixedUpdate,
            .runIn = RunIn::Play,
            .run = [](const SystemContext& context) {
                if (context.application == nullptr) {
                    return;
                }
                ItemCollectionSystem{}.fixedUpdate(context.scene, context.application->physics.contactEvents());
            }};
}
} // namespace

ComponentType collectibleItemComponentType() {
    using Item = CollectibleItemComponent;
    return ComponentTypeBuilder<Item>(std::string(DemoGameplay::CollectibleItemKey), "Collectible Item")
        .category("Gameplay")
        .fields(Fields<Item>().add("value", &Item::value).drag(1.0f, 1.0, 1000000.0))
        .validate(
            [](const Item& item) { return item.value > 0 ? std::string{} : "collectible item value must be positive"; })
        .build();
}

ComponentType itemCollectorComponentType() {
    using Collector = ItemCollectorComponent;
    return ComponentTypeBuilder<Collector>(std::string(DemoGameplay::ItemCollectorKey), "Item Collector")
        .category("Gameplay")
        .fields(Fields<Collector>())
        .build();
}

void registerDemoGameplay(ModuleRegistrar& registrar) {
    registrar.addComponentType(collectibleItemComponentType());
    registrar.addComponentType(itemCollectorComponentType());

    registrar.addInputActions(*createDemoInputMap());
    registrar.addSystem(itemCollectionSystem());
}
