// A game module with editor extensions, for GameModuleHostTests and the
// editor's module smoke test. It is built twice: the second build defines
// ZENITH_EDITOR_TEST_MODULE_V2, which changes its version and what its play
// system and behaviour add, so a test can hot reload one build over the
// other.
//
// Like any game module it links only the engine: everything it uses from the
// editor is the header-only EditorModuleApi, and it draws with the Dear ImGui
// the engine exports. It uses every editor extension point.

#include "editor/modules/EditorModuleApi.hpp"
#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/ui/GameUi.hpp"

#include <imgui.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
#if defined(ZENITH_EDITOR_TEST_MODULE_V2)
constexpr const char* moduleVersion = "2.0.0";
constexpr int32_t playIncrement = 100;
#else
constexpr const char* moduleVersion = "1.0.0";
constexpr int32_t playIncrement = 1;
#endif
constexpr int32_t bumpAmount = 1000;

struct EditorTestCounter {
    // Play Update adds playIncrement; Edit Update counts editTicks; every
    // FixedUpdate counts fixedTicks; every Play Update or FixedUpdate that
    // received player input counts inputTicks.
    int32_t value{0};
    int32_t editTicks{0};
    int32_t fixedTicks{0};
    int32_t inputTicks{0};
    std::string label{"counter"};
    // Update runs (in any mode) that had a logger, and that had gameplay
    // services. Optional, so documents written before them still load.
    int32_t loggerSeen{0};
    int32_t servicesSeen{0};
};

// A behaviour: each Play update adds playIncrement to turns, and onStart
// counts its starts.
class EditorTestSpinner : public Behaviour {
  public:
    int32_t turns{0};
    int32_t starts{0};

    void onStart(BehaviourContext&) override {
        ++starts;
    }

    void onUpdate(BehaviourContext&) override {
        turns += playIncrement;
    }
};

template <typename Function> void forEachCounter(Scene& scene, Function function) {
    scene.registry().view<EditorTestCounter>().each([&](EditorTestCounter& counter) { function(counter); });
}

// The name of an entity that makes count_play throw.
constexpr const char* faultTrigger = "editor_test_throw";

void throwOnFaultTrigger(const Scene& scene) {
    const auto names = scene.registry().view<const NameComponent>();
    for (const Entity entity : names) {
        if (names.get<const NameComponent>(entity).name == faultTrigger) {
            throw std::runtime_error("count_play found the fault trigger");
        }
    }
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
                                               .add("input_ticks", &EditorTestCounter::inputTicks)
                                               .add("label", &EditorTestCounter::label)
                                               .add("logger_seen", &EditorTestCounter::loggerSeen)
                                               .optional()
                                               .add("services_seen", &EditorTestCounter::servicesSeen)
                                               .optional())
                                   .build());
    registrar.addBehaviour<EditorTestSpinner>(
        "editor_test_spinner", "Editor Test Spinner",
        Fields<EditorTestSpinner>().add("turns", &EditorTestSpinner::turns).add("starts", &EditorTestSpinner::starts));
    // Throws while the scene has an entity named faultTrigger, for the fault
    // isolation tests.
    registrar.addSystem({.name = "count_play",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Play,
                         .run = [](const SystemContext& context) {
                             throwOnFaultTrigger(context.scene);
                             forEachCounter(context.scene, [&context](EditorTestCounter& counter) {
                                 counter.value += playIncrement;
                                 counter.inputTicks += context.input != nullptr ? 1 : 0;
                             });
                         }});
    registrar.addSystem({.name = "count_edit",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::Edit,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene, [](EditorTestCounter& counter) { ++counter.editTicks; });
                         }});
    registrar.addSystem({.name = "probe_context",
                         .phase = SystemPhase::Update,
                         .runIn = RunIn::EditAndPlay,
                         .order = 10,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene, [&context](EditorTestCounter& counter) {
                                 counter.loggerSeen += context.logger != nullptr ? 1 : 0;
                                 counter.servicesSeen += context.services != nullptr ? 1 : 0;
                             });
                         }});
    // The game's HUD: the counter's value.
    registrar.addSystem(
        {.name = "hud", .phase = SystemPhase::Ui, .runIn = RunIn::Play, .run = [](const SystemContext& context) {
             if (context.ui == nullptr) {
                 return;
             }
             forEachCounter(context.scene, [&context](const EditorTestCounter& counter) {
                 context.ui->text({16.0f, 16.0f}, "HUD " + std::to_string(counter.value));
             });
         }});
    registrar.addSystem({.name = "count_fixed",
                         .phase = SystemPhase::FixedUpdate,
                         .runIn = RunIn::EditAndPlay,
                         .run = [](const SystemContext& context) {
                             forEachCounter(context.scene, [&context](EditorTestCounter& counter) {
                                 ++counter.fixedTicks;
                                 counter.inputTicks += context.input != nullptr ? 1 : 0;
                             });
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
    registrar.addSidebarTab({.title = "Test Tab", .draw = [](EditorModulePanelContext& context) {
                                 ImGui::Text("Editor test sidebar tab: %zu selected", context.selection.size());
                             }});
    // Replaces the counter's reflected fields. Bump (or a label of "bump",
    // which lets a headless test request it) adds bumpAmount to the value and
    // relabels the counter, editing two top-level keys at once.
    registrar.addInspectorOverride(
        {.componentKey = "editor_test_counter", .draw = [](EditorModuleInspectorContext& context) {
             EditorTestCounter& counter = context.get<EditorTestCounter>();
             ImGui::Text("Editor test inspector: %d", counter.value);
             if (!ImGui::Button("Bump") && counter.label != "bump") {
                 return false;
             }
             counter.value += bumpAmount;
             counter.label = "bumped";
             return true;
         }});
    // Menus: the module's own, offered as a submenu of the Add menu.
    registrar.addMenu({.id = "EDITOR_TEST_MT_tools", .title = "Editor Test"});
    registrar.addMenuEntry(
        {.menu = "EDITOR_TEST_MT_tools", .label = "Add Test Counter", .operatorId = "editor_test.add_counter"});
    registrar.addMenuEntry({.menu = EditorModuleMenus::add, .label = "Editor Test", .submenu = "EDITOR_TEST_MT_tools"});
}
