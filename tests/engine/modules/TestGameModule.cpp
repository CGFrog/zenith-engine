// A game module for GameModuleTests: one reflected component type and systems
// whose effects on that component reveal their order, phase, and modes.

#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <string>

namespace {
struct TestCounterComponent {
    // Written by the play-mode Update systems: "add_one" (order -1) then
    // "double" (order 0) turn 0 into 2, while the reverse order gives 1.
    int32_t value{0};
    int32_t editTicks{0};
    int32_t fixedTicks{0};
    float elapsedSeconds{0.0f};
    std::string label{"counter"};
};

template <typename Function> void forEachCounter(const SystemContext& context, Function function) {
    context.scene.registry().view<TestCounterComponent>().each(
        [&](TestCounterComponent& counter) { function(counter); });
}
} // namespace

ZENITH_GAME_MODULE("zenith_test_game", "1.0.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registrar.addComponentType(ComponentTypeBuilder<TestCounterComponent>("test_counter", "Test Counter")
                                   .category("Test")
                                   .fields(Fields<TestCounterComponent>()
                                               .add("value", &TestCounterComponent::value)
                                               .add("edit_ticks", &TestCounterComponent::editTicks)
                                               .add("fixed_ticks", &TestCounterComponent::fixedTicks)
                                               .add("elapsed_seconds", &TestCounterComponent::elapsedSeconds)
                                               .add("label", &TestCounterComponent::label))
                                   .build());

    // Registered before "add_one" so only the order field can run it second.
    registrar.addSystem({.name = "double",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Play,
                         .order = 0,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [&](TestCounterComponent& counter) {
                                 counter.value *= 2;
                                 counter.elapsedSeconds += context.deltaSeconds;
                             });
                         }});
    registrar.addSystem({.name = "add_one",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Play,
                         .order = -1,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [](TestCounterComponent& counter) { ++counter.value; });
                         }});
    registrar.addSystem({.name = "edit_tick",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Edit,
                         .order = 5,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [](TestCounterComponent& counter) { ++counter.editTicks; });
                         }});
    registrar.addSystem({.name = "fixed_tick",
                         .phase = SystemPhase::FixedUpdate,
                         .runIn = RunIn::EditAndPlay,
                         .order = 0,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [](TestCounterComponent& counter) { ++counter.fixedTicks; });
                         }});
}
