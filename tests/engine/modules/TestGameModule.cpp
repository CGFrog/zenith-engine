// A game module for GameModuleTests: one reflected component type and systems
// whose effects on that component reveal their order, phase, and modes.

#include "engine/module/GameEvents.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/ui/GameUi.hpp"

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
    // Optional, so documents written before they existed still load.
    int32_t lateTicks{0};
    // Update runs (in any mode) that had a logger, and that had services.
    int32_t loggerSeen{0};
    int32_t servicesSeen{0};
    // Presses of the module's Fire action seen by Update and by FixedUpdate.
    int32_t firePresses{0};
    int32_t fixedFirePresses{0};
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
                                               .add("label", &TestCounterComponent::label)
                                               .add("late_ticks", &TestCounterComponent::lateTicks)
                                               .optional()
                                               .add("logger_seen", &TestCounterComponent::loggerSeen)
                                               .optional()
                                               .add("services_seen", &TestCounterComponent::servicesSeen)
                                               .optional()
                                               .add("fire_presses", &TestCounterComponent::firePresses)
                                               .optional()
                                               .add("fixed_fire_presses", &TestCounterComponent::fixedFirePresses)
                                               .optional())
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
    InputActionMap actions;
    actions.addAction("Fire", InputActionValueType::Button);
    actions.addBinding("Fire", {.source = Key::F});
    registrar.addInputActions(actions);
    registrar.addSystem(
        {.name = "fire",
         .phase = SystemPhase::Update,
         .runIn = RunIn::Play,
         .order = 20,
         .run = [](const SystemContext& context) {
             const bool fired = context.actions != nullptr && context.actions->pressed("Fire");
             forEachCounter(context, [fired](TestCounterComponent& counter) { counter.firePresses += fired ? 1 : 0; });
         }});
    registrar.addSystem({.name = "fixed_fire",
                         .phase = SystemPhase::FixedUpdate,
                         .runIn = RunIn::Play,
                         .order = 20,
                         .run = [](const SystemContext& context) {
                             const bool fired = context.actions != nullptr && context.actions->pressed("Fire");
                             forEachCounter(context, [fired](TestCounterComponent& counter) {
                                 counter.fixedFirePresses += fired ? 1 : 0;
                             });
                         }});
    registrar.addSystem({.name = "late_tick",
                         .phase = SystemPhase::LateUpdate,
                         .runIn = RunIn::Play,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [](TestCounterComponent& counter) { ++counter.lateTicks; });
                         }});
    // Draws the counter's value as the HUD.
    registrar.addSystem(
        {.name = "hud", .phase = SystemPhase::Ui, .runIn = RunIn::Play, .run = [](const SystemContext& context) {
             if (context.ui == nullptr) {
                 return;
             }
             forEachCounter(context, [&](const TestCounterComponent& counter) {
                 context.ui->text({16.0f, 16.0f}, "HUD " + std::to_string(counter.value));
             });
         }});
    // Records what the context offers, and answers the engine's SceneLoaded
    // event (published by the host, read here in module code) by spawning an
    // entity through the deferred commands.
    registrar.addSystem({.name = "context_probe",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::EditAndPlay,
                         .order = 10,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [&](TestCounterComponent& counter) {
                                 counter.loggerSeen += context.logger != nullptr ? 1 : 0;
                                 counter.servicesSeen += context.services != nullptr ? 1 : 0;
                             });
                             if (context.services != nullptr &&
                                 !context.services->events().read<SceneLoaded>().empty()) {
                                 context.services->commands().spawnEmpty("Spawned on load");
                             }
                         }});
    registrar.addSystem({.name = "fixed_tick",
                         .phase = SystemPhase::FixedUpdate,
                         .runIn = RunIn::EditAndPlay,
                         .order = 0,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context, [](TestCounterComponent& counter) { ++counter.fixedTicks; });
                         }});
}
