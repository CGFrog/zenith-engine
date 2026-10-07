// The System Profiler's editor side, headless: the play operators that set the
// time scale and the Simulate mask, the saved Simulate choices, and the
// profiler panel drawn over a real game module's systems.

#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/PlayOperators.hpp"
#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/panels/SystemProfilerPanel.hpp"
#include "editor/settings/SimulateSystemSettings.hpp"
#include "engine/diagnostics/FrameTimings.hpp"
#include "engine/input/InputManager.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Runs operators as the session does and records which ran.
class RecordingActions final : public TestSessionActions {
  public:
    using TestSessionActions::TestSessionActions;

    OperatorResult invokeOperator(ApplicationContext& app, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        invoked.emplace_back(operatorId);
        return TestSessionActions::invokeOperator(app, operatorId, properties, region);
    }

    std::vector<std::string> invoked;
};

// What the session owns around the profiler, without a window or GPU.
struct ProfilerFixture : EditorTestFixture {
    EngineFrameTimings frameTimings;
    EditorPlayMode playMode;
    GameModuleHost host;
    int maskSaves{0};
    RecordingActions actions{operators, runner, sessionContextSource()};

    ProfilerFixture()
        : EditorTestFixture({.logger = {.minimumLevel = LogLevel::Debug, .echoToStderr = false}})
        , host({.operators = operators,
                .runner = runner,
                .menus = menus.registry(),
                .viewports = viewports,
                .componentEditors = componentEditors},
               {.shadowDirectory = scratch / "shadow"}) {
        app.frameTimings = &frameTimings;
        followedPlayMode = &playMode;
        useActions(actions);
        CHECK_MESSAGE(
            (registerPlayOperators(
                 operators, {.playMode = playMode, .modules = host, .simulateMaskChanged = [this] { ++maskSaves; }})
                 .success),
            "the play operators register");
    }

    ~ProfilerFixture() {
        host.unload(scene, document);
    }
};

// Loads the editor test module into the fixture's host; false (and a failed
// check) when it does not load.
bool loadTestModule(ProfilerFixture& fixture) {
    const GameModuleHostResult loaded = fixture.host.load(ZENITH_EDITOR_TEST_MODULE_V1, fixture.document);
    CHECK_MESSAGE(loaded.success, "the editor test module loads");
    if (!loaded) {
        MESSAGE(loaded.error);
        return false;
    }
    return true;
}

} // namespace

TEST_SUITE_BEGIN("SystemProfiler");

TEST_CASE("SystemProfiler: settings") {
    const testsupport::TempDirectory directory("system-profiler-settings");
    const std::filesystem::path path = directory / "settings" / "simulate-systems.json";
    SimulateSystemMask loaded;
    loaded.set("stale", true);
    CHECK_MESSAGE((loadSimulateSystemSettings(path, loaded).success && loaded.choices().empty()),
                  "a missing settings file loads as no choices");

    SimulateSystemMask mask;
    mask.set("follow_camera", true);
    mask.set("move_platforms", false);
    CHECK_MESSAGE(saveSimulateSystemSettings(path, mask).success, "Simulate choices save");
    CHECK_MESSAGE((loadSimulateSystemSettings(path, loaded).success && loaded == mask),
                  "Simulate choices load back unchanged");

    for (const std::string_view invalid :
         {std::string_view("not json"), std::string_view(R"({"version":2,"systems":{}})"),
          std::string_view(R"({"version":1,"systems":{"a":1}})"), std::string_view(R"({"version":1,"systems":[]})")}) {
        testsupport::writeText(path, invalid);
        loaded.set("stale", true);
        const SimulateSystemSettingsResult result = loadSimulateSystemSettings(path, loaded);
        CHECK_MESSAGE(
            (!result && result.error.find("simulate-systems.json") != std::string::npos && loaded.choices().empty()),
            "an invalid settings file reports its path and leaves the defaults");
    }
}

TEST_CASE("SystemProfiler: operators without module") {
    ProfilerFixture fixture;
    CHECK_MESSAGE((fixture.invoke("play.set_time_scale", R"({"scale":0.25})").status == OperatorStatus::Finished &&
                   fixture.playMode.timeScale() == 0.25f),
                  "play.set_time_scale sets the time scale");
    CHECK_MESSAGE((fixture.invoke("play.set_time_scale", R"({"scale":25})").status == OperatorStatus::Finished &&
                   fixture.playMode.timeScale() == 10.0f),
                  "play.set_time_scale clamps to ten");
    CHECK_MESSAGE(fixture.playMode.simulationClock().timeScale == 1.0f,
                  "Edit mode runs at real time whatever the time scale");
    CHECK_MESSAGE(fixture.invoke("play.set_time_scale", R"({"scale":1})").status == OperatorStatus::Finished,
                  "the time scale returns to real time");
    CHECK_MESSAGE((fixture.invoke("play.set_simulate_system", R"({"system":"count_play","enabled":true})").status ==
                       OperatorStatus::Failed &&
                   fixture.maskSaves == 0),
                  "choosing a system fails while the module has none");
    CHECK_MESSAGE(fixture.invoke("profiler.reset_max").status == OperatorStatus::Finished,
                  "resetting the maxima works without a module");
    CHECK_MESSAGE(fixture.history.size() == 0, "play operators record no undo steps");
}

TEST_CASE("SystemProfiler: with module") {
    ProfilerFixture fixture;
    if (!loadTestModule(fixture)) {
        return;
    }
    CHECK_MESSAGE((fixture.invoke("play.set_simulate_system", R"({"system":"count_play","enabled":true})").status ==
                       OperatorStatus::Finished &&
                   fixture.host.simulateMask().enabled("count_play", SystemPhase::Update) && fixture.maskSaves == 1),
                  "play.set_simulate_system enables a module system for Simulate and saves the choice");
    CHECK_MESSAGE(fixture.invoke("play.set_simulate_system", R"({"system":"missing"})").status ==
                      OperatorStatus::Failed,
                  "an unknown system is refused");
    CHECK_MESSAGE((fixture.operators.find("play.set_simulate_system") != nullptr &&
                   hasFlag(fixture.operators.find("play.set_simulate_system")->flags, OperatorFlags::Internal)),
                  "choosing a system needs a name, so it is hidden from search");

    // A few frames of systems for the panel to show.
    fixture.host.runEditFrame(fixture.scene, 0.1f, 1.0f / 60.0f);
    fixture.host.finishSystemFrame();
    fixture.frameTimings = {.frame = 1, .totalMilliseconds = 16.0, .updateMilliseconds = 2.0, .fixedSteps = 1};

    // The refused operators above reported their errors.
    fixture.diagnostics.acknowledgeErrors();
    SystemProfilerPanel panel(fixture.host, fixture.playMode);
    CHECK_MESSAGE(std::string_view(panel.title()) == "System Profiler", "the panel is titled System Profiler");
    HeadlessImGuiContext imgui;
    for (int frame = 0; frame < 2; ++frame) {
        EditorContext editor = fixture.context();
        HeadlessImGuiFrame imguiFrame(panel.title());
        panel.draw(editor);
    }
    CHECK_MESSAGE(fixture.actions.invoked.empty(), "drawing the profiler runs no operator by itself");
    CHECK_MESSAGE(!fixture.diagnostics.hasUnacknowledgedError(), "drawing the profiler reports no errors");
}

// The Simulate system picker (shared by the profiler, the Play menu, and the
// playback toolbar) lists the Play systems with their mask state, and a
// checkbox click toggles the mask through play.set_simulate_system.
TEST_CASE("SystemProfiler: simulate picker") {
    ProfilerFixture fixture;
    if (!loadTestModule(fixture)) {
        return;
    }
    const std::vector<SimulateSystemChoice> choices = simulateSystemChoices(fixture.host);
    CHECK_MESSAGE(!choices.empty(), "the test module has systems Simulate can run");
    if (choices.empty()) {
        return;
    }
    for (const SimulateSystemChoice& choice : choices) {
        CHECK_MESSAGE(choice.enabled == fixture.host.simulateMask().enabled(choice.name, choice.phase),
                      "each choice shows the mask's state");
    }
    const SimulateSystemChoice first = choices.front();
    fixture.actions.invoked.clear();
    HeadlessImGuiContext imgui;
    const char* window = "Picker";
    const auto draw = [&] {
        EditorContext editor = fixture.context();
        HeadlessImGuiFrame frame(window);
        drawSimulateSystemPicker(editor, fixture.host);
    };
    draw();
    const ImGuiWindow* pickerWindow = HeadlessImGuiContext::findWindow(window);
    CHECK_MESSAGE(pickerWindow != nullptr, "the picker draws into its window");
    if (pickerWindow == nullptr) {
        return;
    }
    const std::string label = simulateSystemLabel(first);
    HeadlessImGuiContext::activateNextFrame(
        HeadlessImGuiContext::itemId(*pickerWindow, first.name.c_str(), label.c_str()));
    draw();
    draw();
    CHECK_MESSAGE((fixture.actions.invoked == std::vector<std::string>{"play.set_simulate_system"} &&
                   fixture.host.simulateMask().enabled(first.name, first.phase) != first.enabled),
                  "clicking a picker checkbox toggles the system in the Simulate mask");
}

TEST_SUITE_END();
