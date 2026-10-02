// A game module with editor extensions, for GameModuleHostTests and the
// editor's module smoke test. It is built twice: the second build defines
// ZENITH_EDITOR_TEST_MODULE_V2, which changes its version and what its play
// system adds, so a test can hot reload one build over the other.
//
// Like any game module it links only the engine: everything it uses from the
// editor is the header-only EditorModuleApi.

#include "editor/modules/EditorModuleApi.hpp"
#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <string>

namespace {
#if defined(ZENITH_EDITOR_TEST_MODULE_V2)
constexpr const char* moduleVersion = "2.0.0";
constexpr int32_t playIncrement = 100;
#else
constexpr const char* moduleVersion = "1.0.0";
constexpr int32_t playIncrement = 1;
#endif

struct EditorTestCounter {
    // Play Update adds playIncrement; Edit Update counts editTicks; every
    // FixedUpdate counts fixedTicks.
    int32_t value{0};
    int32_t editTicks{0};
    int32_t fixedTicks{0};
    std::string label{"counter"};
};

template <typename Function> void forEachCounter(Scene& scene, Function function) {
    scene.registry().view<EditorTestCounter>().each([&](EditorTestCounter& counter) { function(counter); });
}
} // namespace

ZENITH_GAME_MODULE("zenith_editor_test_game", moduleVersion);

void zenith_module_register(ModuleRegistrar& registrar) {
    registrar.addComponentType(ComponentTypeBuilder<EditorTestCounter>("editor_test_counter", "Editor Test Counter")
                                   .category("Test")
                                   .fields(Fields<EditorTestCounter>()
                                               .add("value", &EditorTestCounter::value)
                                               .add("edit_ticks", &EditorTestCounter::editTicks)
                                               .add("fixed_ticks", &EditorTestCounter::fixedTicks)
                                               .add("label", &EditorTestCounter::label))
                                   .build());
    registrar.addSystem({.name = "count_play",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Play,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene,
                                            [](EditorTestCounter& counter) { counter.value += playIncrement; });
                         }});
    registrar.addSystem({.name = "count_edit",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Edit,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene, [](EditorTestCounter& counter) { ++counter.editTicks; });
                         }});
    registrar.addSystem({.name = "count_fixed",
                         .phase = SystemPhase::FixedUpdate,
                         .runIn = RunIn::EditAndPlay,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene, [](EditorTestCounter& counter) { ++counter.fixedTicks; });
                         }});
}

ZENITH_EDITOR_MODULE_REGISTER {
    // Adds a counter to every selected entity, as one undoable step.
    registrar.addOperator({.id = "editor_test.add_counter",
                           .label = "Add Test Counter",
                           .description = "Add an Editor Test Counter to the selected entities",
                           .poll = [](const EditorModuleContext& context) { return !context.selection.empty(); },
                           .execute = [](EditorModuleContext& context) -> EditorModuleResult {
                               for (const SceneEntityId id : context.selection) {
                                   const Entity entity = context.scene.findEntity(id);
                                   if (entity == entt::null) {
                                       return {.error = "A selected entity no longer exists"};
                                   }
                                   context.scene.registry().emplace_or_replace<EditorTestCounter>(entity);
                               }
                               return {.success = true};
                           }});
    // Rejected: the id is taken, which the host reports as a warning.
    registrar.addOperator({.id = "editor_test.add_counter", .label = "Duplicate", .execute = [](EditorModuleContext&) {
                               return EditorModuleResult{.success = true};
                           }});
    registrar.addPanel({.title = "Test Counters", .draw = [](EditorModulePanelContext&) {}});
}
