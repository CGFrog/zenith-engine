// The editor's game module host without a window: loading the test module and
// its editor extensions, edit, play, and Simulate systems, gameplay sessions
// and system faults, module operators and previews, hot reload, a failed
// reload that preserves module data, and unloading.

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

#include "support/Doctest.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

// The host also lists the engine's own systems ("zenith." names).
size_t moduleSystemCount(std::span<const SystemDescriptor* const> systems) {
    return static_cast<size_t>(std::ranges::count_if(
        systems, [](const SystemDescriptor* system) { return !system->name.starts_with("zenith."); }));
}

constexpr std::string_view counterKey = "editor_test_counter";
constexpr std::string_view spinnerKey = "editor_test_spinner";
constexpr std::string_view addCounterOperator = "editor_test.add_counter";
constexpr std::string_view generatePreviewOperator = "editor_test.generate_preview";
constexpr std::string_view previewChannel = "editor_test.preview";
constexpr std::string_view previewRootName = "Editor Test Preview";
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
        CHECK_MESSAGE(registerDefaultMenus(menus.registry()).success, "the editor's menus register");
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
    CHECK_MESSAGE(!error, "a module build copies over the project's module file");
    std::filesystem::last_write_time(destination, std::filesystem::file_time_type::clock::now(), error);
}

bool directoryEmpty(const std::filesystem::path& directory) {
    std::error_code error;
    return !std::filesystem::exists(directory, error) || std::filesystem::is_empty(directory, error);
}

// A project of the case's own whose module file is a copy of the test
// module's first build, with the directory the host keeps its copies in.
struct ModuleProject {
    explicit ModuleProject(std::string_view label)
        : directory(label)
        , paths{.root = directory.path(),
                .shadow = directory / "shadow",
                .module = directory / "source" / std::filesystem::path(ZENITH_EDITOR_TEST_MODULE_V1).filename()} {
        std::filesystem::create_directories(paths.root / "source");
        installModule(ZENITH_EDITOR_TEST_MODULE_V1, paths.module);
    }

    testsupport::TempDirectory directory;
    TestPaths paths;
};

// A host over its own project. The module is unloaded before the editor
// objects go, because the scene can hold storage the module's code created;
// the project (a base constructed first) outlives the host's module copies.
struct ModuleHostFixture : ModuleProject, HostFixture {
    explicit ModuleHostFixture(std::string_view label)
        : ModuleProject(label)
        , HostFixture(paths) {}

    ~ModuleHostFixture() {
        host.unload(scene, document);
    }

    ModuleHostFixture(const ModuleHostFixture&) = delete;
    ModuleHostFixture& operator=(const ModuleHostFixture&) = delete;
};

// The test module loaded, with a counter on one selected entity in the state
// "load edit and play" leaves it: value 6 and label "kept" after one Play
// update, one edit tick, five fixed ticks, and adding it the only undo step.
struct LoadedModuleFixture : ModuleHostFixture {
    explicit LoadedModuleFixture(std::string_view label)
        : ModuleHostFixture(label) {
        const GameModuleHostResult loaded = host.load(paths.module, document);
        REQUIRE_MESSAGE(loaded.success, ("the editor test module loads: " + loaded.error));
        counterId = scene.id(scene.createEntity("Counter"));
        selection.selectOnly(counterId);
        REQUIRE_MESSAGE(invoke(addCounterOperator).status == OperatorStatus::Finished,
                        "the module operator adds a counter to the selection");
        *counterField<int32_t>(counterId, "value") = 5;
        *counterField<std::string>(counterId, "label") = "kept";
        host.runSystems(SystemPhase::Update, scene, nullptr, SimulationMode::Play, 0.016f);
        host.runEditFrame(scene, 1.0f, 0.25f);
        host.runSystems(SystemPhase::FixedUpdate, scene, nullptr, SimulationMode::Play, 0.25f);
    }

    SceneEntityId counterId;
};

} // namespace

TEST_SUITE_BEGIN("GameModuleHost");

TEST_CASE("GameModuleHost: without module") {
    ModuleHostFixture fixture("game-module-host-without-module");
    const TestPaths& paths = fixture.paths;
    CHECK_MESSAGE((fixture.host.status() == GameModuleStatus::None && fixture.host.module() == nullptr),
                  "a host starts without a module");
    CHECK_MESSAGE(!fixture.host.reload(fixture.scene, fixture.assets, fixture.document),
                  "reloading without a configured module fails");
    CHECK_MESSAGE(!fixture.host.pollSourceChange(std::chrono::steady_clock::now()),
                  "a host without a module never reports a change");
    fixture.host.runEditFrame(fixture.scene, 1.0f, 0.25f);

    const GameModuleHostResult missing = fixture.host.load(paths.root / "missing_module.dll", fixture.document);
    CHECK_MESSAGE((!missing && missing.error.find("not found") != std::string::npos),
                  "a missing module fails to load with a descriptive error");
    CHECK_MESSAGE((fixture.host.status() == GameModuleStatus::Failed && fixture.host.error() == missing.error),
                  "a failed load leaves the host failed with the error");
    CHECK_MESSAGE((fixture.document.options().componentTypes == nullptr && fixture.counterType() == nullptr),
                  "after a failed load the document uses the built-in component types");
    CHECK_MESSAGE(fixture.extensionsReleased(), "a host without a module adds nothing to the editor's registries");
}

TEST_CASE("GameModuleHost: load edit and play") {
    ModuleHostFixture fixture("game-module-host-load");
    const TestPaths& paths = fixture.paths;
    SceneEntityId counterId;
    const GameModuleHostResult loaded = fixture.host.load(paths.module, fixture.document);
    INFO("load error: " << loaded.error);
    CHECK_MESSAGE(loaded.success, "the editor test module loads");
    if (!loaded) {
        return;
    }
    CHECK_MESSAGE((fixture.host.status() == GameModuleStatus::Loaded && fixture.host.module()->version() == "1.0.0"),
                  "the host reports the loaded module and its version");
    CHECK_MESSAGE(
        (fixture.counterType() != nullptr && componentTypesFor(fixture.document.options()).find("point_light")),
        "the document reads module components alongside the built-in types");
    CHECK_MESSAGE((fixture.host.operatorIds().size() == 2 && fixture.operators.find(addCounterOperator) != nullptr &&
                   fixture.operators.find(generatePreviewOperator) != nullptr),
                  "the module's editor operators are registered");
    CHECK_MESSAGE(
        (loaded.warnings.size() == 2 && loaded.warnings[0].find("already registered") != std::string::npos &&
         loaded.warnings[1].find("declared twice") != std::string::npos &&
         fixture.operators.find("editor_test.bad_properties") == nullptr),
        "rejected editor registrations, a duplicate id and a duplicate property key, are reported as warnings");
    CHECK_MESSAGE((fixture.host.panels().size() == 1 && fixture.host.panels()[0].panel.title == "Test Counters"),
                  "the module's editor panel is registered");

    const Entity entity = fixture.scene.createEntity("Counter");
    counterId = fixture.scene.id(entity);
    CHECK_MESSAGE(!fixture.invoke(addCounterOperator), "the module operator does not run without a selection");
    fixture.selection.selectOnly(counterId);
    CHECK_MESSAGE((fixture.invoke(addCounterOperator).status == OperatorStatus::Finished &&
                   fixture.counterField<int32_t>(counterId, "value") != nullptr),
                  "the module operator adds a module component to the selection");
    CHECK_MESSAGE(fixture.history.canUndo(), "the module operator is recorded as an undo step");
    if (fixture.counterField<int32_t>(counterId, "value") == nullptr) {
        return;
    }

    *fixture.counterField<int32_t>(counterId, "value") = 5;
    *fixture.counterField<std::string>(counterId, "label") = "kept";
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == 6 && fixture.counterValue(counterId, "edit_ticks") == 0),
                  "play mode runs the module's play systems only");
    fixture.host.runEditFrame(fixture.scene, 1.0f, 0.25f);
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == 6 && fixture.counterValue(counterId, "edit_ticks") == 1),
                  "edit mode runs the module's edit systems only");
    CHECK_MESSAGE(fixture.counterValue(counterId, "fixed_ticks") == 4,
                  "edit mode runs edit fixed-update systems once per elapsed fixed step");
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Play, 0.25f);
    CHECK_MESSAGE(fixture.counterValue(counterId, "fixed_ticks") == 5, "play fixed updates run fixed-update systems");
}

// Edit systems get a logger but no gameplay services; a gameplay session gives
// Play systems both, and its Ui phase draws into the given game UI.
TEST_CASE("GameModuleHost: gameplay session") {
    LoadedModuleFixture fixture("game-module-host-gameplay");
    const SceneEntityId counterId = fixture.counterId;
    const int32_t loggerSeen = fixture.counterValue(counterId, "logger_seen");
    const int32_t servicesSeen = fixture.counterValue(counterId, "services_seen");
    const int32_t debugDrawSeen = fixture.counterValue(counterId, "debug_draw_seen");
    fixture.host.runEditFrame(fixture.scene, 0.016f, 0.0f);
    CHECK_MESSAGE((fixture.counterValue(counterId, "logger_seen") == loggerSeen + 1 &&
                   fixture.counterValue(counterId, "services_seen") == servicesSeen),
                  "edit systems get a logger and no gameplay services");
    CHECK_MESSAGE(fixture.counterValue(counterId, "debug_draw_seen") == debugDrawSeen + 1,
                  "edit systems get the host's debug drawing");

    DebugDraw debugDraw;
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    CHECK_MESSAGE(fixture.host.gameServices() != nullptr, "a gameplay session has services");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    CHECK_MESSAGE((fixture.counterValue(counterId, "logger_seen") == loggerSeen + 2 &&
                   fixture.counterValue(counterId, "services_seen") == servicesSeen + 1),
                  "play systems get a logger and the session's services");
    fixture.host.runEditFrame(fixture.scene, 0.016f, 0.0f);
    CHECK_MESSAGE(fixture.counterValue(counterId, "services_seen") == servicesSeen + 1,
                  "edit systems get no services even during a session");

    GameUiContext ui;
    ui.beginFrame({640, 360}, InputState{});
    fixture.host.runSystems(SystemPhase::Ui, fixture.scene, nullptr, SimulationMode::Play, 0.0f, nullptr, &ui);
    const std::string expected = "HUD " + std::to_string(fixture.counterValue(counterId, "value"));
    CHECK_MESSAGE(std::ranges::any_of(ui.commands(),
                                      [&expected](const UiCommand& command) {
                                          const auto* text = std::get_if<UiTextCommand>(&command.payload);
                                          return text != nullptr && text->text == expected;
                                      }),
                  "Ui systems draw the HUD into the game UI they are given");

    fixture.host.stopGameplay(fixture.scene);
    CHECK_MESSAGE(fixture.host.gameServices() == nullptr, "stopping the session releases its services");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    CHECK_MESSAGE(fixture.counterValue(counterId, "services_seen") == servicesSeen + 1,
                  "play systems outside a session get no services");
}

// A module system that throws in a gameplay session is disabled for the rest
// of it while the other systems keep running; the next session runs it again.
TEST_CASE("GameModuleHost: system faults") {
    LoadedModuleFixture fixture("game-module-host-faults");
    const SceneEntityId counterId = fixture.counterId;
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t loggerSeen = fixture.counterValue(counterId, "logger_seen");
    DebugDraw debugDraw;
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    // count_play throws while this entity exists.
    const Entity trigger = fixture.scene.createEntity("editor_test_throw");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    const SystemFaults& faults = fixture.host.systemFaults();
    CHECK_MESSAGE((faults.disabled("count_play") && faults.faults().size() == 1 &&
                   faults.faults()[0].what.find("fault trigger") != std::string::npos),
                  "a throwing module system is disabled with what it threw");
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == value &&
                   fixture.counterValue(counterId, "logger_seen") == loggerSeen + 2),
                  "the other systems of the phase keep running every frame");

    fixture.host.stopGameplay(fixture.scene);
    CHECK_MESSAGE(fixture.host.systemFaults().empty(), "stopping the session enables the system again");
    fixture.scene.destroyEntity(trigger);
    fixture.host.startGameplay(fixture.assets, fixture.document.options(), debugDraw);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == value + 1 && fixture.host.systemFaults().empty()),
                  "the next session runs the system again");
    fixture.host.stopGameplay(fixture.scene);
}

// The module's menu entry, sidebar tab, and inspector override, used the way
// the editor uses them.
TEST_CASE("GameModuleHost: editor extensions") {
    LoadedModuleFixture fixture("game-module-host-extensions");
    CHECK_MESSAGE(fixture.extensionsRegistered(),
                  "the module's sidebar tab, inspector override, and menus are registered");
    const GameModuleHost::ExtensionCounts counts = fixture.host.extensionCounts();
    CHECK_MESSAGE(
        (counts.sidebarTabs == 1 && counts.inspectorOverrides == 1 && counts.menus == 1 && counts.menuEntries == 2),
        "the host records every extension the module registered");
    InspectorFixture inspector(fixture);

    // Add > Editor Test > Add Test Counter runs the module operator on two
    // new entities.
    const SceneEntityId first = fixture.scene.id(fixture.scene.createEntity("Bumped"));
    const SceneEntityId second = fixture.scene.id(fixture.scene.createEntity("Copied"));
    fixture.selection.assign({first, second});
    const std::optional<ResolvedMenu> add = fixture.addMenu();
    const MenuItem* submenu = add ? HostFixture::findItem(add->items, "Editor Test") : nullptr;
    const MenuItem* entry = submenu != nullptr ? HostFixture::findItem(submenu->children, "Add Test Counter") : nullptr;
    CHECK_MESSAGE((entry != nullptr && entry->operatorId == addCounterOperator),
                  "the module's submenu of the Add menu resolves to its operator");
    if (entry == nullptr) {
        return;
    }
    OperatorContext menuContext = fixture.operatorContext();
    CHECK_MESSAGE((fixture.runner.invokeWithJson(entry->operatorId, menuContext, entry->properties).status ==
                       OperatorStatus::Finished &&
                   fixture.counterField<int32_t>(first, "value") != nullptr &&
                   fixture.counterField<int32_t>(second, "value") != nullptr),
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
    CHECK_MESSAGE(
        (drawn.find("Editor test inspector") != std::string::npos && drawn.find("Edit Ticks") == std::string::npos),
        "the Inspector draws the module's override instead of the reflected fields");
    CHECK_MESSAGE((fixture.counterValue(first, "value") == 5 + bumpAmount &&
                   *fixture.counterField<std::string>(first, "label") == "bumped"),
                  "the override edits the active entity's component");
    CHECK_MESSAGE((fixture.counterValue(second, "value") == 5 + bumpAmount &&
                   *fixture.counterField<std::string>(second, "label") == "bumped" &&
                   fixture.counterValue(second, "edit_ticks") == 3),
                  "the view copies only the keys the override changed to the other selected entity");
    CHECK_MESSAGE(fixture.history.size() == historyBeforeEdit + 1, "the override's edit is recorded as one undo step");
    fixture.editing().undo();
    CHECK_MESSAGE((fixture.counterValue(first, "value") == 5 && fixture.counterValue(second, "value") == 7 &&
                   *fixture.counterField<std::string>(first, "label") == "bump"),
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
    CHECK_MESSAGE((firstTab != std::string::npos &&
                   tabText.find("Editor test sidebar tab: 2 selected", firstTab + 1) != std::string::npos),
                  "the module's sidebar tab draws with the selection in each viewport");
}

namespace {

// The preview root the generate operator builds, or null.
Entity findPreviewRoot(const Scene& scene) {
    const auto names = scene.registry().view<const NameComponent>();
    for (const Entity entity : names) {
        if (names.get<const NameComponent>(entity).name == previewRootName) {
            return entity;
        }
    }
    return entt::null;
}

size_t previewRootCount(const Scene& scene) {
    const auto names = scene.registry().view<const NameComponent>();
    return static_cast<size_t>(std::ranges::count_if(
        names, [&names](Entity entity) { return names.get<const NameComponent>(entity).name == previewRootName; }));
}

// The counter values of the preview's children, in child order; empty without
// a preview.
std::vector<int32_t> previewValues(HostFixture& fixture) {
    std::vector<int32_t> values;
    const Entity root = findPreviewRoot(fixture.scene);
    if (root == entt::null) {
        return values;
    }
    for (const Entity child : fixture.scene.registry().get<RelationshipComponent>(root).children) {
        values.push_back(fixture.counterValue(fixture.scene.id(child), "value"));
    }
    return values;
}

} // namespace

// "Generate (seed)": a module operator with properties builds a transient
// preview with the edit services; Adjust Last Operation runs it again with
// another seed, deterministically, without touching the undo history.
TEST_CASE("GameModuleHost: generate preview") {
    LoadedModuleFixture fixture("game-module-host-preview");
    // As the session does: module code draws where the viewports render.
    DebugDraw& debugDraw = fixture.rendering.debugDraw();
    fixture.host.setDebugDraw(debugDraw);
    const Operator* op = fixture.operators.find(generatePreviewOperator);
    CHECK_MESSAGE((op != nullptr && hasFlag(op->flags, OperatorFlags::Register) && op->properties.fields().size() == 7),
                  "a module operator with properties is registered for Adjust Last Operation");
    const std::vector<SceneEntityId> selectionBefore = fixture.selection.ids();
    const size_t historyBefore = fixture.history.size();
    std::string savedBefore;
    SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), savedBefore);

    CHECK_MESSAGE(fixture.invoke(generatePreviewOperator, R"({"seed": 5})").status == OperatorStatus::Finished,
                  "the generate operator runs with a seed from JSON");
    const Entity root = findPreviewRoot(fixture.scene);
    const std::vector<int32_t> seedFive = previewValues(fixture);
    CHECK_MESSAGE((root != entt::null && fixture.scene.isTransient(root) && seedFive.size() == 3),
                  "the operator builds a transient preview with the default count");
    std::string savedAfter;
    SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), savedAfter);
    CHECK_MESSAGE((savedAfter == savedBefore && fixture.history.size() == historyBefore),
                  "the transient preview is neither saved nor recorded for undo");
    CHECK_MESSAGE((debugDraw.lineCount() == 3 * 12 && debugDraw.labels().size() == 3 && !debugDraw.channels().empty() &&
                   debugDraw.channels()[0].name == previewChannel),
                  "the operator draws into the host's debug draw channel");
    debugDraw.endFrame(10.0f);
    CHECK_MESSAGE(debugDraw.lineCount() == 3 * 12, "the preview outline is persistent");
    CHECK_MESSAGE(fixture.host.editRandom().seed() == 5, "the operator reseeds the host's random streams");

    const LastOperator* last = fixture.runner.lastOperator();
    CHECK_MESSAGE((last != nullptr && last->id == generatePreviewOperator &&
                   last->properties.toJson().find("\"spawned\":3") != std::string::npos),
                  "Adjust Last Operation remembers the run with the value the operator wrote back");
    if (last == nullptr) {
        return;
    }
    OperatorProperties adjusted = last->properties;
    CHECK_MESSAGE(adjusted.applyJson(R"({"seed": 9})").empty(), "the seed is an editable property");
    OperatorContext context = fixture.operatorContext();
    CHECK_MESSAGE(fixture.runner.adjustLast(context, adjusted).status == OperatorStatus::Finished,
                  "Adjust Last Operation runs the operator again with a new seed");
    const std::vector<int32_t> seedNine = previewValues(fixture);
    CHECK_MESSAGE((previewRootCount(fixture.scene) == 1 && seedNine.size() == 3 && seedNine != seedFive),
                  "the re-run replaces the preview with the new seed's");
    CHECK_MESSAGE(debugDraw.lineCount() == 3 * 12, "the re-run clears the channel before drawing again");
    last = fixture.runner.lastOperator();
    if (last != nullptr) {
        adjusted = last->properties;
        adjusted.applyJson(R"({"seed": 5})");
        OperatorContext again = fixture.operatorContext();
        fixture.runner.adjustLast(again, adjusted);
    }
    CHECK_MESSAGE(previewValues(fixture) == seedFive, "the same seed generates the same preview");
    CHECK_MESSAGE(fixture.history.size() == historyBefore, "adjusting a preview records nothing either");

    // Every property type reaches the module.
    CHECK_MESSAGE(
        (fixture.invoke(generatePreviewOperator, R"({"count": 2, "layout": "ring", "labels": false, "spacing": 4.0,
                                                       "prefix": "Cell "})")
                 .status == OperatorStatus::Finished &&
         previewValues(fixture).size() == 2 && debugDraw.labels().empty()),
        "int, enum, bool, float, and string properties reach the operator");
    const Entity ringRoot = findPreviewRoot(fixture.scene);
    const Entity firstCell = ringRoot != entt::null
                                 ? fixture.scene.registry().get<RelationshipComponent>(ringRoot).children.front()
                                 : Entity{entt::null};
    CHECK_MESSAGE((firstCell != entt::null && fixture.scene.registry().get<NameComponent>(firstCell).name == "Cell 0" &&
                   std::abs(fixture.scene.localPosition(firstCell).x - 4.0f) < 1e-4f),
                  "the ring layout and prefix were used");
    CHECK_MESSAGE(fixture.invoke(generatePreviewOperator, R"({"layout": "spiral"})").status == OperatorStatus::Failed,
                  "an enum value that is not an option is rejected");
    CHECK_MESSAGE((fixture.invoke(generatePreviewOperator, R"({"count": -1})").status == OperatorStatus::Failed &&
                   !fixture.diagnostics.entries().empty() &&
                   fixture.diagnostics.entries().back().message.find("count of zero") != std::string::npos),
                  "the module's own failure is reported");

    // A panel invokes the operator with values through the panel context.
    const std::vector<int32_t> before = previewValues(fixture);
    debugDraw.endFrame();
    const size_t linesBefore = debugDraw.lineCount();
    EditorContext editor = fixture.context();
    bool invoked = false;
    drawModulePanelContents(editor, [&invoked](EditorModulePanelContext& panel) {
        EditorModuleProperties values;
        values.set("seed", int32_t{9});
        invoked = panel.invokeOperator(generatePreviewOperator, values);
        panel.debugDraw.channel("editor_test.panel_probe").line({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    });
    CHECK_MESSAGE((invoked && previewValues(fixture) == seedNine),
                  "a panel invokes a module operator with property values");
    const std::vector<DebugDrawChannelInfo> channels = debugDraw.channels();
    CHECK_MESSAGE((debugDraw.lineCount() == 3 * 12 + 1 && linesBefore == 2 * 12 &&
                   std::ranges::any_of(channels,
                                       [](const DebugDrawChannelInfo& channel) {
                                           return channel.name == "editor_test.panel_probe" && channel.active;
                                       })),
                  "a panel draws into the session's debug draw");
    CHECK_MESSAGE(before.size() == 2, "the previous run's preview was replaced");

    fixture.scene.destroyTransient();
    debugDraw.clear();
    fixture.runner.forgetLast();
    fixture.selection.assign(selectionBefore);
}

// Simulate runs FixedUpdate systems and the Update systems the user enabled,
// never passes input, and every run is timed in the host's profile.
TEST_CASE("GameModuleHost: simulate") {
    LoadedModuleFixture fixture("game-module-host-simulate");
    const SceneEntityId counterId = fixture.counterId;
    const InputState input;
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t fixedTicks = fixture.counterValue(counterId, "fixed_ticks");
    const int32_t inputTicks = fixture.counterValue(counterId, "input_ticks");
    CHECK_MESSAGE((moduleSystemCount(fixture.host.systems(SystemPhase::Update)) == 3 &&
                   moduleSystemCount(fixture.host.systems(SystemPhase::FixedUpdate)) == 1 &&
                   moduleSystemCount(fixture.host.systems(SystemPhase::Ui)) == 1),
                  "the host lists the module's systems by phase");

    fixture.host.finishSystemFrame();
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    fixture.host.finishSystemFrame();
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == value &&
                   fixture.counterValue(counterId, "fixed_ticks") == fixedTicks + 2),
                  "Simulate runs FixedUpdate systems but no Update system by default");
    CHECK_MESSAGE(fixture.counterValue(counterId, "input_ticks") == inputTicks,
                  "Simulate never passes player input to systems, even when the caller has some");
    const SystemTiming* fixed = fixture.host.systemProfile().find("count_fixed");
    const SystemTiming* play = fixture.host.systemProfile().find("count_play");
    CHECK_MESSAGE((fixed != nullptr && fixed->frameRuns == 2 && fixed->phase == SystemPhase::FixedUpdate),
                  "the profile counts every fixed run of the frame");
    CHECK_MESSAGE((play == nullptr || play->frameRuns == 0), "a system Simulate skipped has no runs that frame");

    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Play, 0.25f, &input);
    CHECK_MESSAGE(fixture.counterValue(counterId, "input_ticks") == inputTicks + 1, "Play passes input to systems");

    fixture.host.setSimulateSystemEnabled("count_play", true);
    fixture.host.setSimulateSystemEnabled("count_fixed", false);
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f, &input);
    fixture.host.runSystems(SystemPhase::FixedUpdate, fixture.scene, nullptr, SimulationMode::Simulate, 0.25f, &input);
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == value + 1 &&
                   fixture.counterValue(counterId, "fixed_ticks") == fixedTicks + 3 &&
                   fixture.counterValue(counterId, "input_ticks") == inputTicks + 1),
                  "the Simulate mask chooses which systems Simulate runs, still without input");
    fixture.host.setSimulateSystemEnabled("count_fixed", true);
    fixture.host.finishSystemFrame();
    fixture.host.resetSystemTimingMaxima();
    CHECK_MESSAGE(fixture.host.systemProfile().find("count_fixed")->maximumMilliseconds == 0.0,
                  "resetting the maxima clears them");
}

TEST_CASE("GameModuleHost: hot reload") {
    LoadedModuleFixture fixture("game-module-host-hot-reload");
    const TestPaths& paths = fixture.paths;
    const SceneEntityId counterId = fixture.counterId;
    // As "simulate" leaves it: Simulate runs count_play too.
    fixture.host.setSimulateSystemEnabled("count_play", true);
    const auto start = std::chrono::steady_clock::now();
    CHECK_MESSAGE((!fixture.host.pollSourceChange(start) && !fixture.host.sourceChanged()),
                  "an unchanged module file is not reported");
    installModule(ZENITH_EDITOR_TEST_MODULE_V2, paths.module);
    CHECK_MESSAGE(fixture.host.sourceChanged(), "a rebuilt module file is detected");
    CHECK_MESSAGE(!fixture.host.pollSourceChange(start + std::chrono::milliseconds(10)),
                  "a change is not reported until the file settles");
    CHECK_MESSAGE(fixture.host.pollSourceChange(start + std::chrono::milliseconds(200)),
                  "a settled change is reported once");
    CHECK_MESSAGE(!fixture.host.pollSourceChange(start + std::chrono::milliseconds(210)),
                  "a reported change is not reported again before the next poll cycle");

    for (GameModuleHost::Panel& panel : fixture.host.panels()) {
        panel.visible = true;
    }
    // A behaviour's fields survive the reload.
    const Entity spinner = fixture.scene.createEntity("Spinner");
    const SceneEntityId spinnerId = fixture.scene.id(spinner);
    componentTypesFor(fixture.document.options()).find(spinnerKey)->addDefault(fixture.scene, spinner);
    *fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") = 7;
    // A preview and the edit services' state do not outlive the module.
    CHECK_MESSAGE((fixture.invoke(generatePreviewOperator, R"({"seed": 11})").status == OperatorStatus::Finished &&
                   fixture.host.editRandom().seed() == 11),
                  "a preview exists before the reload");
    const GameModuleHostResult reloaded = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    CHECK_MESSAGE((findPreviewRoot(fixture.scene) == entt::null),
                  "the reload restores the scene from its document, without the transient preview");
    RandomStreams fresh;
    CHECK_MESSAGE((fixture.host.editRandom().seed() == 0 &&
                   fixture.host.editRandom().stream("layout").state() == fresh.stream("layout").state()),
                  "the reload starts the module's random streams over");
    CHECK_MESSAGE(fixture.runner.lastOperator() == nullptr, "the reload forgets the module operator's last run");
    CHECK_MESSAGE((reloaded.success && fixture.host.module() != nullptr && fixture.host.module()->version() == "2.0.0"),
                  "the rebuilt module hot reloads");
    CHECK_MESSAGE((fixture.host.loadCount() == 2 && !fixture.host.sourceChanged()), "the reload loads the new file");
    CHECK_MESSAGE((fixture.counterValue(counterId, "value") == 6 &&
                   fixture.counterValue(counterId, "fixed_ticks") == 5 &&
                   fixture.counterField<std::string>(counterId, "label") != nullptr &&
                   *fixture.counterField<std::string>(counterId, "label") == "kept"),
                  "module component values survive the reload");
    fixture.selection.removeMissing(fixture.scene);
    CHECK_MESSAGE(fixture.selection.ids() == std::vector<SceneEntityId>{counterId},
                  "the selection survives the reload");
    CHECK_MESSAGE((fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") != nullptr &&
                   *fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") == 7),
                  "behaviour fields survive the reload");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    CHECK_MESSAGE(fixture.counterValue(counterId, "value") == 106, "the reloaded module's systems run");
    CHECK_MESSAGE((*fixture.componentField<int32_t>(spinnerKey, spinnerId, "turns") == 107 &&
                   *fixture.componentField<int32_t>(spinnerKey, spinnerId, "starts") == 1),
                  "the reloaded module's behaviour runs its new code");
    CHECK_MESSAGE((fixture.operators.find(addCounterOperator) != nullptr && fixture.host.panels().size() == 1),
                  "the reloaded module's editor extensions are registered again");
    CHECK_MESSAGE((fixture.extensionsRegistered() && reloaded.warnings.size() == 2),
                  "the reload released every extension, so registering them again succeeds once each");
    CHECK_MESSAGE((!fixture.host.panels().empty() && fixture.host.panels()[0].visible),
                  "an open module panel stays open");
    // The engine's behaviour, navigation, and animator systems, count_play,
    // and probe_context ran once since the reload.
    CHECK_MESSAGE(fixture.host.systemProfile().timings().size() == 5,
                  "a reload starts the profile over with the new module's systems");
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Simulate, 0.016f);
    CHECK_MESSAGE((fixture.host.simulateMask().enabled("count_play", SystemPhase::Update) &&
                   fixture.counterValue(counterId, "value") == 206),
                  "the Simulate mask survives a module reload");

    fixture.editing().undo();
    CHECK_MESSAGE(fixture.counterField<int32_t>(counterId, "value") == nullptr,
                  "undo history recorded before reload undoes");
    fixture.editing().redo();
    CHECK_MESSAGE(fixture.counterField<int32_t>(counterId, "value") != nullptr,
                  "undo history recorded before reload redoes");
    // Redo restored the counter as the operator added it.
    if (int32_t* value = fixture.counterField<int32_t>(counterId, "value")) {
        *value = 106;
        *fixture.counterField<std::string>(counterId, "label") = "kept";
    }
}

TEST_CASE("GameModuleHost: failed reload preserves data") {
    LoadedModuleFixture fixture("game-module-host-failed-reload");
    const TestPaths& paths = fixture.paths;
    const SceneEntityId counterId = fixture.counterId;
    // As "hot reload" leaves it: the counter ran once more after the reload,
    // and the module's panel is open.
    *fixture.counterField<int32_t>(counterId, "value") = 106;
    for (GameModuleHost::Panel& panel : fixture.host.panels()) {
        panel.visible = true;
    }
    testsupport::writeText(paths.module, "not a shared library");
    const GameModuleHostResult failed = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    CHECK_MESSAGE((!failed && !failed.error.empty() && fixture.host.status() == GameModuleStatus::Failed),
                  "a reload of a broken library fails with an error");
    CHECK_MESSAGE((fixture.document.options().componentTypes == nullptr && fixture.counterType() == nullptr),
                  "after a failed reload the document uses the built-in types");
    CHECK_MESSAGE((fixture.host.operatorIds().empty() && fixture.operators.find(addCounterOperator) == nullptr &&
                   fixture.host.panels().empty()),
                  "a failed reload removes the module's editor extensions");
    CHECK_MESSAGE(fixture.extensionsReleased(),
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
    CHECK_MESSAGE(counterPreserved, "module components are kept as JSON when the module cannot load");
    std::string saved;
    CHECK_MESSAGE((SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), saved).success &&
                   saved.find(counterKey) != std::string::npos),
                  "saving without the module writes its preserved components back");

    installModule(ZENITH_EDITOR_TEST_MODULE_V1, paths.module);
    const GameModuleHostResult recovered = fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    CHECK_MESSAGE((recovered.success && fixture.host.module()->version() == "1.0.0"),
                  "a fixed module reloads after failure");
    CHECK_MESSAGE(fixture.extensionsRegistered(), "a fixed module registers its editor extensions again");
    CHECK_MESSAGE((!fixture.host.panels().empty() && fixture.host.panels()[0].visible),
                  "a panel open before a failed reload reopens when the module loads again");
    CHECK_MESSAGE(fixture.counterValue(counterId, "value") == 106, "preserved module data becomes components again");
    const Entity restored = fixture.scene.findEntity(counterId);
    CHECK_MESSAGE((restored != entt::null && !fixture.scene.registry().all_of<PreservedComponentsComponent>(restored)),
                  "nothing stays preserved once the module reads it");
}

TEST_CASE("GameModuleHost: unload") {
    LoadedModuleFixture fixture("game-module-host-unload");
    const TestPaths& paths = fixture.paths;
    // A second build hot reloaded, so the host has made more than one copy.
    installModule(ZENITH_EDITOR_TEST_MODULE_V2, paths.module);
    fixture.host.reload(fixture.scene, fixture.assets, fixture.document);
    fixture.host.unload(fixture.scene, fixture.document);
    CHECK_MESSAGE((fixture.host.status() == GameModuleStatus::None && fixture.host.module() == nullptr),
                  "unloading leaves the host without a module");
    CHECK_MESSAGE((fixture.scene.registry().view<SceneEntityIdComponent>().empty() &&
                   fixture.document.options().componentTypes == nullptr &&
                   fixture.operators.find(addCounterOperator) == nullptr),
                  "unloading releases the scene, the document's registry, and the editor extensions");
    CHECK_MESSAGE(fixture.extensionsReleased(),
                  "unloading removes every editor extension from the editor's registries");
    CHECK_MESSAGE(directoryEmpty(paths.shadow),
                  "every module copy is unloaded and deleted, so nothing still referenced it");
}

TEST_SUITE_END();
