#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr std::string_view counterKey = "editor_test_counter";
constexpr std::string_view addCounterOperator = "editor_test.add_counter";

struct TestPaths {
    std::filesystem::path root;
    std::filesystem::path shadow;
    // Where the "project" expects its module; builds are copied over it.
    std::filesystem::path module;
};

// The editor objects the session owns around the host, without a window.
struct HostFixture {
    AssetManager assets;
    Scene scene;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    GameModuleHost host;

    explicit HostFixture(const TestPaths& paths)
        : host(operators, runner,
               {.shadowDirectory = paths.shadow,
                .settleTime = std::chrono::milliseconds(100),
                .checkInterval = std::chrono::milliseconds(0)}) {}

    SceneEditing editing() {
        return {scene, assets, document, history, diagnostics};
    }

    OperatorResult invoke(std::string_view id) {
        OperatorContext context{.editing = editing(), .selection = selection};
        return runner.invoke(id, context);
    }

    const ComponentType* counterType() const {
        return componentTypesFor(document.options()).find(counterKey);
    }

    // The counter's field, or null when the entity has no counter.
    template <typename Value> Value* counterField(SceneEntityId id, std::string_view key) {
        const ComponentType* type = counterType();
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

// Simulate runs FixedUpdate systems and the Update systems the user enabled,
// never passes input, and every run is timed in the host's profile.
void testSimulate(HostFixture& fixture, SceneEntityId counterId) {
    const InputState input;
    const int32_t value = fixture.counterValue(counterId, "value");
    const int32_t fixedTicks = fixture.counterValue(counterId, "fixed_ticks");
    const int32_t inputTicks = fixture.counterValue(counterId, "input_ticks");
    expect(fixture.host.systems(SystemPhase::Update).size() == 2 &&
               fixture.host.systems(SystemPhase::FixedUpdate).size() == 1,
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
    fixture.host.runSystems(SystemPhase::Update, fixture.scene, nullptr, SimulationMode::Play, 0.016f);
    expect(fixture.counterValue(counterId, "value") == 106, "the reloaded module's systems run");
    expect(fixture.operators.find(addCounterOperator) != nullptr && fixture.host.panels().size() == 1,
           "the reloaded module's editor extensions are registered again");
    expect(!fixture.host.panels().empty() && fixture.host.panels()[0].visible, "an open module panel stays open");
    expect(fixture.host.systemProfile().timings().size() == 1,
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
