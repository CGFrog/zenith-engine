#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The host also lists the engine's own systems ("zenith." names).
size_t moduleSystemCount(std::span<const SystemDescriptor* const> systems) {
    return static_cast<size_t>(std::ranges::count_if(
        systems, [](const SystemDescriptor* system) { return !system->name.starts_with("zenith."); }));
}

constexpr std::string_view counterKey = "editor_test_counter";
constexpr std::string_view spinnerKey = "editor_test_spinner";
constexpr std::string_view addCounterOperator = "editor_test.add_counter";
constexpr std::string_view moduleMenu = "EDITOR_TEST_MT_tools";
constexpr std::string_view moduleSidebarTab = "Test Tab";
constexpr int32_t bumpAmount = 1000;

struct TestPaths {
    std::filesystem::path root;
    std::filesystem::path shadow;
    // Where the "project" expects its module; builds are copied over it.
    std::filesystem::path module;
};

// The editor objects the session owns around the host, without a window.
struct HostFixture : EditorTestFixture {
    GameModuleHost host;

    explicit HostFixture(const TestPaths& paths)
        : host({.operators = operators,
                .runner = runner,
                .menus = menus.registry(),
                .viewports = viewports,
                .componentEditors = componentEditors},
               {.shadowDirectory = paths.shadow,
                .settleTime = std::chrono::milliseconds(100),
                .checkInterval = std::chrono::milliseconds(0)}) {
        expect(registerDefaultMenus(menus.registry()).success, "the editor's menus register");
    }

    // The item of items labelled label, or null.
    static const MenuItem* findItem(const std::vector<MenuItem>& items, std::string_view label) {
        const auto found = std::ranges::find_if(items, [label](const MenuItem& item) { return item.label == label; });
        return found == items.end() ? nullptr : &*found;
    }

    std::optional<ResolvedMenu> addMenu() {
        const OperatorContext context = operatorContext();
        return menus.resolve({.menu = addMenuId, .category = {}}, context);
    }

    bool hasSidebarTab() const {
        return std::ranges::any_of(viewports.sidebarTabs(),
                                   [](const ViewportSidebarTab& tab) { return tab.title == moduleSidebarTab; });
    }

    // Whether no extension of the module is left in the editor's registries.
    bool extensionsReleased() {
        const std::optional<ResolvedMenu> add = addMenu();
        return operators.find(addCounterOperator) == nullptr && componentEditors.size() == 0 && !hasSidebarTab() &&
               menus.registry().find(moduleMenu) == nullptr && add && findItem(add->items, "Editor Test") == nullptr &&
               host.extensionCounts() == GameModuleHost::ExtensionCounts{};
    }

    // Whether the module's extensions are each registered exactly once.
    bool extensionsRegistered() {
        const std::optional<ResolvedMenu> add = addMenu();
        const MenuItem* submenu = add ? findItem(add->items, "Editor Test") : nullptr;
        return operators.find(addCounterOperator) != nullptr && componentEditors.find(counterKey) != nullptr &&
               componentEditors.size() == 1 && viewports.sidebarTabs().size() == 1 && hasSidebarTab() &&
               menus.registry().find(moduleMenu) != nullptr && submenu != nullptr && submenu->children.size() == 1 &&
               host.missingExtensions().empty();
    }

    const ComponentType* counterType() const {
        return componentTypesFor(document.options()).find(counterKey);
    }

    // The counter's field, or null when the entity has no counter.
    template <typename Value> Value* counterField(SceneEntityId id, std::string_view key) {
        return componentField<Value>(counterKey, id, key);
    }

    // The field of the entity's component of type componentKey, or null.
    template <typename Value>
    Value* componentField(std::string_view componentKey, SceneEntityId id, std::string_view key) {
        const ComponentType* type = componentTypesFor(document.options()).find(componentKey);
        const Entity entity = scene.findEntity(id);
        void* counter = type != nullptr && entity != entt::null ? type->find(scene, entity) : nullptr;
        if (counter == nullptr) {
            return nullptr;
        }
        for (const FieldDescriptor& field : type->fields()) {
            if (field.key == key) {
                return &field.value<Value>(counter);
            }
        }
        return nullptr;
    }

    int32_t counterValue(SceneEntityId id, std::string_view key) {
        const int32_t* value = counterField<int32_t>(id, key);
        return value != nullptr ? *value : -1;
    }
};

// What the Inspector draws with: the fixture's editor context, with its
// module's component editors, and ImGui without a window.
struct InspectorFixture {
    HostFixture& host;
    EntityPropertiesView view{host.componentLayout};
    HeadlessImGuiContext imgui;

    explicit InspectorFixture(HostFixture& fixture)
        : host(fixture) {}

    // Runs draw in one frame and returns the text it rendered.
    template <typename Draw> std::string drawLogged(Draw draw) {
        HeadlessImGuiFrame frame("Inspector");
        ImGui::LogToBuffer();
        EditorContext editor = host.context();
        draw(editor);
        std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        return text;
    }
};

// Replaces the module file as a build would, with a fresh write time so the
// change is visible even when the copies were linked in the same second.
void installModule(const std::filesystem::path& build, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::copy_file(build, destination, std::filesystem::copy_options::overwrite_existing, error);
    expect(!error, "a module build copies over the project's module file");
    std::filesystem::last_write_time(destination, std::filesystem::file_time_type::clock::now(), error);
}

bool directoryEmpty(const std::filesystem::path& directory) {
    std::error_code error;
    return !std::filesystem::exists(directory, error) || std::filesystem::is_empty(directory, error);
}

void testWithoutModule(const TestPaths& paths) {
    HostFixture fixture(paths);
    expect(fixture.host.status() == GameModuleStatus::None && fixture.host.module() == nullptr,
           "a host starts without a module");
    expect(!fixture.host.reload(fixture.scene, fixture.assets, fixture.document),
           "reloading without a configured module fails");
    expect(!fixture.host.pollSourceChange(std::chrono::steady_clock::now()),
           "a host without a module never reports a change");
    fixture.host.runEditFrame(fixture.scene, 1.0f, 0.25f);

    const GameModuleHostResult missing = fixture.host.load(paths.root / "missing_module.dll", fixture.document);
    expect(!missing && missing.error.find("not found") != std::string::npos,
           "a missing module fails to load with a descriptive error");
    expect(fixture.host.status() == GameModuleStatus::Failed && fixture.host.error() == missing.error,
           "a failed load leaves the host failed with the error");
    expect(fixture.document.options().componentTypes == nullptr && fixture.counterType() == nullptr,
           "after a failed load the document uses the built-in component types");
    expect(fixture.extensionsReleased(), "a host without a module adds nothing to the editor's registries");
}

void testLoadEditAndPlay(HostFixture& fixture, const TestPaths& paths, SceneEntityId& counterId) {
    const GameModuleHostResult loaded = fixture.host.load(paths.module, fixture.document);
    expect(loaded.success, "the editor test module loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    expect(fixture.host.status() == GameModuleStatus::Loaded && fixture.host.module()->version() == "1.0.0",
           "the host reports the loaded module and its version");
    expect(fixture.counterType() != nullptr && componentTypesFor(fixture.document.options()).find("point_light"),
           "the document reads module components alongside the built-in types");
    expect(fixture.host.operatorIds().size() == 1 && fixture.operators.find(addCounterOperator) != nullptr,
           "the module's editor operator is registered");
    expect(loaded.warnings.size() == 1 && loaded.warnings[0].find("already registered") != std::string::npos,
           "a rejected editor registration is reported as a warning");
    expect(fixture.host.panels().size() == 1 && fixture.host.panels()[0].panel.title == "Test Counters",
           "the module's editor panel is registered");

    const Entity entity = fixture.scene.createEntity("Counter");
    counterId = fixture.scene.id(entity);
    expect(!fixture.invoke(addCounterOperator), "the module operator does not run without a selection");
    fixture.selection.selectOnly(counterId);
    expect(fixture.invoke(addCounterOperator).status == OperatorStatus::Finished &&
               fixture.counterField<int32_t>(counterId, "value") != nullptr,
           "the module operator adds a module component to the selection");
    expect(fixture.history.canUndo(), "the module operator is recorded as an undo step");
    if (fixture.counterField<int32_t>(counterId, "value") == nullptr) {
        return;
    }

    *fixture.counterField<int32_t>(counterId, "value") = 5;
    *fixture.counterField<std::string>(counterId, "label") = "kept";
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "value") == 6 && fixture.counterValue(counterId, "edit_ticks") == 0,
           "play mode runs the module's play systems only");
    fixture.host.runEditFrame(fixture.scene, 1.0f, 0.25f);
    expect(fixture.counterValue(counterId, "value") == 6 && fixture.counterValue(counterId, "edit_ticks") == 1,
           "edit mode runs the module's edit systems only");
    expect(fixture.counterValue(counterId, "fixed_ticks") == 4,
           "edit mode runs edit fixed-update systems once per elapsed fixed step");
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Play, 0.25f);
    expect(fixture.counterValue(counterId, "fixed_ticks") == 5, "play fixed updates run fixed-update systems");
}

// Edit systems get a logger but no gameplay services; a gameplay session gives
// Play systems both, and its Ui phase draws into the given game UI.
void testGameplaySession(HostFixture& fixture, SceneEntityId counterId) {
    // Restored at the end: the later tests count from it.
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t loggerSeen = fixture.counterValue(counterId, "logger_seen");
    const int32_t servicesSeen = fixture.counterValue(counterId, "services_seen");
    fixture.host.runEditFrame(fixture.scene, 0.016f, 0.0f);
    expect(fixture.counterValue(counterId, "logger_seen") == loggerSeen + 1 &&
               fixture.counterValue(counterId, "services_seen") == servicesSeen,
           "edit systems get a logger and no gameplay services");

    DebugDraw debugDraw;
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    expect(fixture.host.gameServices() != nullptr, "a gameplay session has services");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "logger_seen") == loggerSeen + 2 &&
               fixture.counterValue(counterId, "services_seen") == servicesSeen + 1,
           "play systems get a logger and the session's services");
    fixture.host.runEditFrame(fixture.scene, 0.016f, 0.0f);
    expect(fixture.counterValue(counterId, "services_seen") == servicesSeen + 1,
           "edit systems get no services even during a session");

    GameUiContext ui;
    ui.beginFrame({640, 360}, InputState{});
    fixture.host.runSystems(SystemPhase::Ui, fixture.scene, nullptr, SimulationMode::Play, 0.0f, nullptr, &ui);
    const std::string expected = "HUD " + std::to_string(fixture.counterValue(counterId, "value"));
    expect(std::ranges::any_of(ui.commands(),
                               [&expected](const UiCommand& command) {
                                   const auto* text = std::get_if<UiTextCommand>(&command.payload);
                                   return text != nullptr && text->text == expected;
                               }),
           "Ui systems draw the HUD into the game UI they are given");

    fixture.host.stopGameplay(fixture.scene);
    expect(fixture.host.gameServices() == nullptr, "stopping the session releases its services");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "services_seen") == servicesSeen + 1,
           "play systems outside a session get no services");
    *fixture.counterField<int32_t>(counterId, "value") = value;
}

// A module system that throws in a gameplay session is disabled for the rest
// of it while the other systems keep running; the next session runs it again.
void testSystemFaults(HostFixture& fixture, SceneEntityId counterId) {
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t loggerSeen = fixture.counterValue(counterId, "logger_seen");
    DebugDraw debugDraw;
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    // count_play throws while this entity exists.
    const Entity trigger = fixture.scene.createEntity("editor_test_throw");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    const SystemFaults& faults = fixture.host.systemFaults();
    expect(faults.disabled("count_play") && faults.faults().size() == 1 &&
               faults.faults()[0].what.find("fault trigger") != std::string::npos,
           "a throwing module system is disabled with what it threw");
    expect(fixture.counterValue(counterId, "value") == value &&
               fixture.counterValue(counterId, "logger_seen") == loggerSeen + 2,
           "the other systems of the phase keep running every frame");

    fixture.host.stopGameplay(fixture.scene);
    expect(fixture.host.systemFaults().empty(), "stopping the session enables the system again");
    fixture.scene.destroyEntity(trigger);
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "value") == value + 1 && fixture.host.systemFaults().empty(),
           "the next session runs the system again");
    fixture.host.stopGameplay(fixture.scene);
    *fixture.counterField<int32_t>(counterId, "value") = value;
    *fixture.counterField<int32_t>(counterId, "logger_seen") = loggerSeen;
}

// The module's menu entry, sidebar tab, and inspector override, used the way
// the editor uses them. Leaves the scene, selection, and undo history as it
// found them (two extra entities without counters aside).
void testEditorExtensions(HostFixture& fixture, SceneEntityId counterId) {
    expect(fixture.extensionsRegistered(), "the module's sidebar tab, inspector override, and menus are registered");
    const GameModuleHost::ExtensionCounts counts = fixture.host.extensionCounts();
    expect(counts.sidebarTabs == 1 && counts.inspectorOverrides == 1 && counts.menus == 1 && counts.menuEntries == 2,
           "the host records every extension the module registered");
    const size_t historyBefore = fixture.history.size();
    InspectorFixture inspector(fixture);

    // Add > Editor Test > Add Test Counter runs the module operator on two
    // new entities.
    const SceneEntityId first = fixture.scene.id(fixture.scene.createEntity("Bumped"));
    const SceneEntityId second = fixture.scene.id(fixture.scene.createEntity("Copied"));
    fixture.selection.assign({first, second});
    const std::optional<ResolvedMenu> add = fixture.addMenu();
    const MenuItem* submenu = add ? HostFixture::findItem(add->items, "Editor Test") : nullptr;
    const MenuItem* entry = submenu != nullptr ? HostFixture::findItem(submenu->children, "Add Test Counter") : nullptr;
    expect(entry != nullptr && entry->operatorId == addCounterOperator,
           "the module's submenu of the Add menu resolves to its operator");
    if (entry == nullptr) {
        return;
    }
    OperatorContext menuContext = fixture.operatorContext();
    expect(fixture.runner.invokeWithJson(entry->operatorId, menuContext, entry->properties).status ==
                   OperatorStatus::Finished &&
               fixture.counterField<int32_t>(first, "value") != nullptr &&
               fixture.counterField<int32_t>(second, "value") != nullptr,
           "the module's menu entry runs its operator");
    if (fixture.counterField<int32_t>(second, "value") == nullptr) {
        return;
    }

    // The override replaces the reflected fields; its edit of the active
    // entity is one undo step, and the changed keys reach the other entity.
    *fixture.counterField<std::string>(first, "label") = "bump";
    *fixture.counterField<int32_t>(first, "value") = 5;
    *fixture.counterField<int32_t>(second, "value") = 7;
    *fixture.counterField<int32_t>(second, "edit_ticks") = 3;
    const size_t historyBeforeEdit = fixture.history.size();
    const std::vector<SceneEntityId> selected = fixture.selection.ids();
    const std::string drawn =
        inspector.drawLogged([&](EditorContext& context) { inspector.view.drawComponents(context, selected, {}); });
    expect(drawn.find("Editor test inspector") != std::string::npos && drawn.find("Edit Ticks") == std::string::npos,
           "the Inspector draws the module's override instead of the reflected fields");
    expect(fixture.counterValue(first, "value") == 5 + bumpAmount &&
               *fixture.counterField<std::string>(first, "label") == "bumped",
           "the override edits the active entity's component");
    expect(fixture.counterValue(second, "value") == 5 + bumpAmount &&
               *fixture.counterField<std::string>(second, "label") == "bumped" &&
               fixture.counterValue(second, "edit_ticks") == 3,
           "the view copies only the keys the override changed to the other selected entity");
    expect(fixture.history.size() == historyBeforeEdit + 1, "the override's edit is recorded as one undo step");
    fixture.editing().undo();
    expect(fixture.counterValue(first, "value") == 5 && fixture.counterValue(second, "value") == 7 &&
               *fixture.counterField<std::string>(first, "label") == "bump",
           "undo restores both entities from before the override's edit");

    // Every viewport draws the module's sidebar tab through the registry.
    const std::span<const ViewportSidebarTab> tabs = fixture.viewports.sidebarTabs();
    const std::string tabText = inspector.drawLogged([&](EditorContext& context) {
        for (int viewport = 0; viewport < 2; ++viewport) {
            for (const ViewportSidebarTab& tab : tabs) {
                tab.draw(context);
            }
        }
    });
    const size_t firstTab = tabText.find("Editor test sidebar tab: 2 selected");
    expect(firstTab != std::string::npos &&
               tabText.find("Editor test sidebar tab: 2 selected", firstTab + 1) != std::string::npos,
           "the module's sidebar tab draws with the selection in each viewport");

    // Leave the history and selection as the later tests expect them: undo
    // the menu entry's step too (the override's step is undone already).
    for (size_t step = 1; step < fixture.history.size() - historyBefore; ++step) {
        fixture.editing().undo();
    }
    fixture.selection.selectOnly(counterId);
}

// Simulate runs FixedUpdate systems and the Update systems the user enabled,
// never passes input, and every run is timed in the host's profile.
void testSimulate(HostFixture& fixture, SceneEntityId counterId) {
    const InputState input;
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t fixedTicks = fixture.counterValue(counterId, "fixed_ticks");
    const int32_t inputTicks = fixture.counterValue(counterId, "input_ticks");
    expect(moduleSystemCount(fixture.host.systems(SystemPhase::Update)) == 3 &&
               moduleSystemCount(fixture.host.systems(SystemPhase::FixedUpdate)) == 1 &&
               moduleSystemCount(fixture.host.systems(SystemPhase::Ui)) == 1,
           "the host lists the module's systems by phase");

    fixture.host.finishSystemFrame();
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    fixture.host.finishSystemFrame();
    expect(fixture.counterValue(counterId, "value") == value &&
               fixture.counterValue(counterId, "fixed_ticks") == fixedTicks + 2,
           "Simulate runs FixedUpdate systems but no Update system by default");
    expect(fixture.counterValue(counterId, "input_ticks") == inputTicks,
           "Simulate never passes player input to systems, even when the caller has some");
    const SystemTiming* fixed = fixture.host.systemProfile().find("count_fixed");
    const SystemTiming* play = fixture.host.systemProfile().find("count_play");
    expect(fixed != nullptr && fixed->frameRuns == 2 && fixed->phase == SystemPhase::FixedUpdate,
           "the profile counts every fixed run of the frame");
    expect(play == nullptr || play->frameRuns == 0, "a system Simulate skipped has no runs that frame");

    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Play, 0.25f, &input);
    expect(fixture.counterValue(counterId, "input_ticks") == inputTicks + 1, "Play passes input to systems");

    fixture.host.setSimulateSystemEnabled("count_play", true);
    fixture.host.setSimulateSystemEnabled("count_fixed", false);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    expect(fixture.counterValue(counterId, "value") == value + 1 &&
               fixture.counterValue(counterId, "fixed_ticks") == fixedTicks + 3 &&
               fixture.counterValue(counterId, "input_ticks") == inputTicks + 1,
           "the Simulate mask chooses which systems Simulate runs, still without input");
    fixture.host.setSimulateSystemEnabled("count_fixed", true);
    fixture.host.finishSystemFrame();
    fixture.host.resetSystemTimingMaxima();
    expect(fixture.host.systemProfile().find("count_fixed")->maximumMilliseconds == 0.0,
           "resetting the maxima clears them");

    // Later tests expect the counter as testLoadEditAndPlay left it.
    *fixture.counterField<int32_t>(counterId, "value") = value;
    *fixture.counterField<int32_t>(counterId, "fixed_ticks") = fixedTicks;
}

void testHotReload(HostFixture& fixture, const TestPaths& paths, SceneEntityId counterId) {
    const auto start = std::chrono::steady_clock::now();
    expect(!fixture.host.pollSourceChange(start) && !fixture.host.sourceChanged(),
           "an unchanged module file is not reported");
    installModule(ZENITH_EDITOR_TEST_MODULE_V2, paths.module);
    expect(fixture.host.sourceChanged(), "a rebuilt module file is detected");
    expect(!fixture.host.pollSourceChange(start + std::chrono::milliseconds(10)),
           "a change is not reported until the file settles");
    expect(fixture.host.pollSourceChange(start + std::chrono::milliseconds(200)), "a settled change is reported once");
    expect(!fixture.host.pollSourceChange(start + std::chrono::milliseconds(210)),
           "a reported change is not reported again before the next poll cycle");

    for (GameModuleHost::Panel& panel : fixture.host.panels()) {
        panel.visible = true;
    }
    // A behaviour's fields survive the reload.
    const Entity spinner = fixture.scene.createEntity("Spinner");
    const SceneEntityId spinnerId = fixture.scene.id(spinner);
    componentTypesFor(fixture.document.options()).find(spinnerKey)->addDefault(fixture.scene, spinner);
    *fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") = 7;
    const GameModuleHostResult reloaded = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    expect(reloaded.success && fixture.host.module() != nullptr && fixture.host.module()->version() == "2.0.0",
           "the rebuilt module hot reloads");
    expect(fixture.host.loadCount() == 2 && !fixture.host.sourceChanged(), "the reload loads the new file");
    expect(fixture.counterValue(counterId, "value") == 6 && fixture.counterValue(counterId, "fixed_ticks") == 5 &&
               fixture.counterField<std::string>(counterId, "label") != nullptr &&
               *fixture.counterField<std::string>(counterId, "label") == "kept",
           "module component values survive the reload");
    fixture.selection.removeMissing(fixture.scene);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{counterId}, "the selection survives the reload");
    expect(fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") != nullptr &&
               *fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") == 7,
           "behaviour fields survive the reload");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "value") == 106, "the reloaded module's systems run");
    expect(*fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") == 107 &&
               *fixture.componentField<int32_t>(spinnerKey, spinnerId, "starts") == 1,
           "the reloaded module's behaviour runs its new code");
    expect(fixture.operators.find(addCounterOperator) != nullptr && fixture.host.panels().size() == 1,
           "the reloaded module's editor extensions are registered again");
    expect(fixture.extensionsRegistered() && reloaded.warnings.size() == 1,
           "the reload released every extension, so registering them again succeeds once each");
    expect(!fixture.host.panels().empty() && fixture.host.panels()[0].visible, "an open module panel stays open");
    // The engine's behaviour, navigation, and animator systems, count_play,
    // and probe_context ran once since the reload.
    expect(fixture.host.systemProfile().timings().size() == 5,
           "a reload starts the profile over with the new module's systems");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f);
    expect(fixture.host.simulateMask().enabled("count_play", SystemPhase::Update) &&
               fixture.counterValue(counterId, "value") == 206,
           "the Simulate mask survives a module reload");

    fixture.editing().undo();
    expect(fixture.counterField<int32_t>(counterId, "value") == nullptr, "undo history recorded before reload undoes");
    fixture.editing().redo();
    expect(fixture.counterField<int32_t>(counterId, "value") != nullptr, "undo history recorded before reload redoes");
    // Redo restored the counter as the operator added it.
    if (int32_t* value = fixture.counterField<int32_t>(counterId, "value")) {
        *value = 106;
        *fixture.counterField<std::string>(counterId, "label") = "kept";
    }
}

void testFailedReloadPreservesData(HostFixture& fixture, const TestPaths& paths, SceneEntityId counterId) {
    {
        std::ofstream broken(paths.module, std::ios::binary | std::ios::trunc);
        broken << "not a shared library";
    }
    const GameModuleHostResult failed = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    expect(!failed && !failed.error.empty() && fixture.host.status() == GameModuleStatus::Failed,
           "a reload of a broken library fails with an error");
    expect(fixture.document.options().componentTypes == nullptr && fixture.counterType() == nullptr,
           "after a failed reload the document uses the built-in types");
    expect(fixture.host.operatorIds().empty() && fixture.operators.find(addCounterOperator) == nullptr &&
               fixture.host.panels().empty(),
           "a failed reload removes the module's editor extensions");
    expect(fixture.extensionsReleased(),
           "a failed reload removes the module's sidebar tab, inspector override, and menus");

    const Entity entity = fixture.scene.findEntity(counterId);
    const auto* preserved =
        entity != entt::null ? fixture.scene.registry().try_get<PreservedComponentsComponent>(entity) : nullptr;
    bool counterPreserved = false;
    if (preserved != nullptr) {
        for (const auto& [key, json] : preserved->components) {
            counterPreserved = counterPreserved || (key == counterKey && json.find("106") != std::string::npos &&
                                                    json.find("kept") != std::string::npos);
        }
    }
    expect(counterPreserved, "module components are kept as JSON when the module cannot load");
    std::string saved;
    expect(SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), saved).success &&
               saved.find(counterKey) != std::string::npos,
           "saving without the module writes its preserved components back");

    installModule(ZENITH_EDITOR_TEST_MODULE_V1, paths.module);
    const GameModuleHostResult recovered = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    expect(recovered.success && fixture.host.module()->version() == "1.0.0", "a fixed module reloads after failure");
    expect(fixture.extensionsRegistered(), "a fixed module registers its editor extensions again");
    expect(!fixture.host.panels().empty() && fixture.host.panels()[0].visible,
           "a panel open before a failed reload reopens when the module loads again");
    expect(fixture.counterValue(counterId, "value") == 106, "preserved module data becomes components again");
    const Entity restored = fixture.scene.findEntity(counterId);
    expect(restored != entt::null && !fixture.scene.registry().all_of<PreservedComponentsComponent>(restored),
           "nothing stays preserved once the module reads it");
}

void testUnload(HostFixture& fixture, const TestPaths& paths) {
    fixture.host.unload(fixture.scene, fixture.document);
    expect(fixture.host.status() == GameModuleStatus::None && fixture.host.module() == nullptr,
           "unloading leaves the host without a module");
    expect(fixture.scene.registry().view<SceneEntityIdComponent>().empty() &&
               fixture.document.options().componentTypes == nullptr &&
               fixture.operators.find(addCounterOperator) == nullptr,
           "unloading releases the scene, the document's registry, and the editor extensions");
    expect(fixture.extensionsReleased(), "unloading removes every editor extension from the editor's registries");
    expect(directoryEmpty(paths.shadow), "every module copy is unloaded and deleted, so nothing still referenced it");
}
} // namespace

int main() {
    TestPaths paths;
    paths.root = std::filesystem::temp_directory_path() / "zenith_game_module_host_tests";
    paths.shadow = paths.root / "shadow";
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
    std::filesystem::create_directories(paths.root / "source", error);
    paths.module = paths.root / "source" / std::filesystem::path(ZENITH_EDITOR_TEST_MODULE_V1).filename();
    installModule(ZENITH_EDITOR_TEST_MODULE_V1, paths.module);

    testWithoutModule(paths);
    {
        HostFixture fixture(paths);
        SceneEntityId counterId;
        testLoadEditAndPlay(fixture, paths, counterId);
        if (fixture.host.module() != nullptr) {
            testGameplaySession(fixture, counterId);
            testSystemFaults(fixture, counterId);
            testEditorExtensions(fixture, counterId);
            testSimulate(fixture, counterId);
            testHotReload(fixture, paths, counterId);
            testFailedReloadPreservesData(fixture, paths, counterId);
        }
        testUnload(fixture, paths);
    }

    std::filesystem::remove_all(paths.root, error);
    if (failures == 0) {
        std::cout << "Game module host tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
