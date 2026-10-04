// The System Profiler's editor side, headless: the play operators that set the
// time scale and the Simulate mask, the saved Simulate choices, and the
// profiler panel drawn over a real game module's systems.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditorOverrides.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PlayOperators.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/SystemProfilerPanel.hpp"
#include "editor/settings/SimulateSystemSettings.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/ui/GameUi.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Runs operators as the session does and records which ran.
class RecordingActions final : public EditorSessionActions {
  public:
    RecordingActions(OperatorRunner& runner, std::function<OperatorContext()> context)
        : m_runner(runner)
        , m_context(std::move(context)) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion) override {
        invoked.emplace_back(operatorId);
        OperatorContext context = m_context();
        return m_runner.invokeWithJson(operatorId, context, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view) override {
        return true;
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}

    std::vector<std::string> invoked;

  private:
    OperatorRunner& m_runner;
    std::function<OperatorContext()> m_context;
};

// What the session owns around the profiler, without a window or GPU.
struct ProfilerFixture {
    Logger logger{{.minimumLevel = LogLevel::Debug, .echoToStderr = false}};
    Scene scene;
    AssetManager assets{&logger};
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    EngineFrameTimings frameTimings;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger, &frameTimings};
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    EditorMenus menus;
    EditorPlayMode playMode;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    ViewportRegistry viewports;
    ComponentEditorOverrides componentEditors;
    GameModuleHost host;
    int maskSaves{0};
    RecordingActions actions{runner, [this] { return operatorContext(); }};

    explicit ProfilerFixture(const std::filesystem::path& shadow)
        : host({.operators = operators,
                .runner = runner,
                .menus = menus.registry(),
                .viewports = viewports,
                .componentEditors = componentEditors},
               {.shadowDirectory = shadow}) {
        expect(registerPlayOperators(
                   operators, {.playMode = playMode, .modules = host, .simulateMaskChanged = [this] { ++maskSaves; }})
                   .success,
               "the play operators register");
    }

    ~ProfilerFixture() {
        host.unload(scene, document);
    }

    ProfilerFixture(const ProfilerFixture&) = delete;
    ProfilerFixture& operator=(const ProfilerFixture&) = delete;

    OperatorContext operatorContext() {
        return {.editing = {scene, assets, document, history, diagnostics},
                .selection = selection,
                .mode = playMode.mode(),
                .app = &app};
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context = operatorContext();
        return runner.invokeWithJson(id, context, properties);
    }

    EditorContext editorContext() {
        return {.app = app,
                .editing = {scene, assets, document, history, diagnostics},
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = playMode.mode(),
                .actions = actions,
                .menus = menus};
    }
};

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void testSettings(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / "settings" / "simulate-systems.json";
    SimulateSystemMask loaded;
    loaded.set("stale", true);
    expect(loadSimulateSystemSettings(path, loaded).success && loaded.choices().empty(),
           "a missing settings file loads as no choices");

    SimulateSystemMask mask;
    mask.set("follow_camera", true);
    mask.set("move_platforms", false);
    expect(saveSimulateSystemSettings(path, mask).success, "Simulate choices save");
    expect(loadSimulateSystemSettings(path, loaded).success && loaded == mask, "Simulate choices load back unchanged");

    for (const std::string_view invalid :
         {std::string_view("not json"), std::string_view(R"({"version":2,"systems":{}})"),
          std::string_view(R"({"version":1,"systems":{"a":1}})"), std::string_view(R"({"version":1,"systems":[]})")}) {
        writeFile(path, invalid);
        loaded.set("stale", true);
        const SimulateSystemSettingsResult result = loadSimulateSystemSettings(path, loaded);
        expect(!result && result.error.find("simulate-systems.json") != std::string::npos && loaded.choices().empty(),
               "an invalid settings file reports its path and leaves the defaults");
    }
}

void testOperatorsWithoutModule(ProfilerFixture& fixture) {
    expect(fixture.invoke("play.set_time_scale", R"({"scale":0.25})").status == OperatorStatus::Finished &&
               fixture.playMode.timeScale() == 0.25f,
           "play.set_time_scale sets the time scale");
    expect(fixture.invoke("play.set_time_scale", R"({"scale":25})").status == OperatorStatus::Finished &&
               fixture.playMode.timeScale() == 10.0f,
           "play.set_time_scale clamps to ten");
    expect(fixture.playMode.simulationClock().timeScale == 1.0f, "Edit mode runs at real time whatever the time scale");
    expect(fixture.invoke("play.set_time_scale", R"({"scale":1})").status == OperatorStatus::Finished,
           "the time scale returns to real time");
    expect(fixture.invoke("play.set_simulate_system", R"({"system":"count_play","enabled":true})").status ==
                   OperatorStatus::Failed &&
               fixture.maskSaves == 0,
           "choosing a system fails while the module has none");
    expect(fixture.invoke("profiler.reset_max").status == OperatorStatus::Finished,
           "resetting the maxima works without a module");
    expect(fixture.history.size() == 0, "play operators record no undo steps");
}

void testWithModule(ProfilerFixture& fixture) {
    const GameModuleHostResult loaded = fixture.host.load(ZENITH_EDITOR_TEST_MODULE_V1, fixture.document);
    expect(loaded.success, "the editor test module loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    expect(fixture.invoke("play.set_simulate_system", R"({"system":"count_play","enabled":true})").status ==
                   OperatorStatus::Finished &&
               fixture.host.simulateMask().enabled("count_play", SystemPhase::Update) && fixture.maskSaves == 1,
           "play.set_simulate_system enables a module system for Simulate and saves the choice");
    expect(fixture.invoke("play.set_simulate_system", R"({"system":"missing"})").status == OperatorStatus::Failed,
           "an unknown system is refused");
    expect(fixture.operators.find("play.set_simulate_system") != nullptr &&
               hasFlag(fixture.operators.find("play.set_simulate_system")->flags, OperatorFlags::Internal),
           "choosing a system needs a name, so it is hidden from search");

    // A few frames of systems for the panel to show.
    fixture.host.runEditFrame(fixture.scene, 0.1f, 1.0f / 60.0f);
    fixture.host.finishSystemFrame();
    fixture.frameTimings = {.frame = 1, .totalMilliseconds = 16.0, .updateMilliseconds = 2.0, .fixedSteps = 1};

    // The refused operators above reported their errors.
    fixture.diagnostics.acknowledgeErrors();
    SystemProfilerPanel panel(fixture.host, fixture.playMode);
    expect(std::string_view(panel.title()) == "System Profiler", "the panel is titled System Profiler");
    HeadlessImGuiContext imgui;
    for (int frame = 0; frame < 2; ++frame) {
        EditorContext editor = fixture.editorContext();
        HeadlessImGuiFrame imguiFrame(panel.title());
        panel.draw(editor);
    }
    expect(fixture.actions.invoked.empty(), "drawing the profiler runs no operator by itself");
    expect(!fixture.diagnostics.hasUnacknowledgedError(), "drawing the profiler reports no errors");
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_system_profiler_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    testSettings(directory);
    {
        ProfilerFixture fixture(directory / "shadow");
        testOperatorsWithoutModule(fixture);
        testWithModule(fixture);
    }

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
