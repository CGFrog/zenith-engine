// The editor's debugging tools: the debugger command and debug.attach, the
// watch list and the debug.* operators that edit it, and the Properties
// panel's Watched section, which shows the active scene's values and runs
// debug.watch_field from a field's context menu.

#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/WatchList.hpp"
#include "editor/operators/DebugOperators.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/settings/RenderSettingsController.hpp"
#include "engine/platform/ChildProcess.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui_internal.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

void testDebuggerCommand() {
    expect(currentProcessId() != 0, "the editor has a process id");
    const ExternalEditorCommandResult expanded = expandDebuggerCommand(R"("my debugger.exe" -p {pid})", 4242);
    expect(expanded && expanded.arguments.size() == 3 && expanded.arguments[0] == "my debugger.exe" &&
               expanded.arguments[2] == "4242",
           "the debugger command substitutes {pid} inside its arguments");
    const ExternalEditorCommandResult wrong = expandDebuggerCommand("gdb {file}", 1);
    expect(!wrong && contains(wrong.error, "debugger command") && contains(wrong.error, "{pid}"),
           "the debugger command names its only placeholder when another is used");
    const ExternalEditorCommandResult ide = expandExternalEditorCommand("code {path}", {"a.cpp", 1, 1});
    expect(!ide && contains(ide.error, "use {file}, {line}, or {column}"),
           "the IDE command's errors are unchanged by the shared expansion");
}

struct DebugFixture : EditorTestFixture {
    EditorToolSettings tools;
    WatchList watchList;

    DebugFixture() {
        // The debugger is cmake itself, so debug.attach launches something
        // real without an installed debugger.
        tools.debugCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo attach {pid}";
        expect(static_cast<bool>(registerDebugOperators(operators, {.tools = tools, .watches = watchList})),
               "the debug operators register");
        watches = &watchList;
    }

    SceneEntityId lamp() {
        const Entity entity = scene.createEntity("Lamp");
        scene.registry().emplace<PointLightComponent>(entity).intensity = 3.5f;
        return scene.id(entity);
    }

    std::string watchProperties(SceneEntityId entity, std::string_view field) {
        return R"({"entity":")" + std::to_string(entity.value) + R"(","component":"point_light","field":")" +
               std::string(field) + R"(","label":"Intensity"})";
    }
};

void testDebugOperators() {
    DebugFixture fixture;
    expect(static_cast<bool>(fixture.invoke("debug.attach")), "debug.attach launches the debugger command");
    fixture.tools.debugCommand = "  ";
    expect(!fixture.operators.find("debug.attach")->canRun(fixture.operatorContext()),
           "debug.attach is unavailable without a debugger command");

    const SceneEntityId lamp = fixture.lamp();
    expect(static_cast<bool>(fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"))) &&
               fixture.watchList.fields().size() == 1 && fixture.watchList.fields()[0].label == "Intensity",
           "debug.watch_field adds a field");
    expect(fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity")).status ==
                   OperatorStatus::Cancelled &&
               fixture.watchList.fields().size() == 1,
           "a field is watched once");
    expect(fixture.invoke("debug.watch_field", R"({"entity":"lamp","component":"point_light","field":"/x"})").status ==
               OperatorStatus::Failed,
           "a bad entity id fails");
    expect(fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "intensity")).status ==
               OperatorStatus::Failed,
           "a field that is not a pointer fails");
    expect(fixture.history.size() == 0, "watching records no undo step");
    expect(static_cast<bool>(fixture.invoke("debug.unwatch_field", fixture.watchProperties(lamp, "/intensity"))) &&
               fixture.watchList.fields().empty(),
           "debug.unwatch_field removes it");
    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"));
    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/range"));
    expect(static_cast<bool>(fixture.invoke("debug.clear_watches")) && fixture.watchList.fields().empty(),
           "debug.clear_watches removes every field");
}

void testWatchedValues() {
    DebugFixture fixture;
    const SceneEntityId lamp = fixture.lamp();
    const SceneDocumentOptions& options = fixture.document.options();
    const auto value = [&](SceneEntityId entity, std::string component, std::string pointer) {
        return watchedValueText(fixture.scene, fixture.assets, options,
                                {.entity = entity, .component = std::move(component), .pointer = std::move(pointer)});
    };
    expect(value(lamp, "point_light", "/intensity") == "3.5", "a watched field shows its value as JSON");
    expect(value(SceneEntityId{987654321}, "point_light", "/intensity") == "(no entity)", "a missing entity is named");
    expect(value(lamp, "spot_light", "/intensity") == "(no component)", "a missing component is named");
    expect(value(lamp, "point_light", "/no_such_field") == "(no field)" &&
               value(lamp, "point_light", "~bad") == "(no field)",
           "a missing or malformed field is named");
}

void testPropertiesPanelWatches() {
    DebugFixture fixture;
    const SceneEntityId lamp = fixture.lamp();
    fixture.selection.selectOnly(lamp);
    RenderSettingsController renderSettings{EditorRenderSettings(
        ProjectConfig{}, std::filesystem::temp_directory_path() / "zenith-debug-tools-render.json")};
    PropertiesPanel panel(renderSettings, fixture.componentLayout);
    HeadlessImGuiContext imgui;
    const auto draw = [&] {
        HeadlessImGuiFrame frame(panel.title());
        EditorContext context = fixture.context();
        panel.draw(context);
    };
    const auto drawLogged = [&] {
        HeadlessImGuiFrame frame(panel.title());
        ImGui::LogToBuffer();
        EditorContext context = fixture.context();
        panel.draw(context);
        std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        return text;
    };
    draw();
    expect(!contains(drawLogged(), "Watched"), "the Watched section is hidden while nothing is watched");

    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"));
    std::string text = drawLogged();
    expect(contains(text, "Watched") && contains(text, "Lamp: Intensity") && contains(text, "3.5"),
           "the Watched section shows the entity, the field, and its value");
    fixture.scene.registry().get<PointLightComponent>(fixture.scene.findEntity(lamp)).intensity = 8.0f;
    expect(contains(drawLogged(), "8.0"), "the value follows the scene every frame");

    fixture.scene.destroyEntity(fixture.scene.findEntity(lamp));
    text = drawLogged();
    expect(contains(text, "(missing): Intensity") && contains(text, "(no entity)"),
           "a watch on a destroyed entity stays, saying so");
    std::filesystem::remove(std::filesystem::temp_directory_path() / "zenith-debug-tools-render.json");
}
} // namespace

int main() {
    testDebuggerCommand();
    testDebugOperators();
    testWatchedValues();
    testPropertiesPanelWatches();
    if (failures != 0) {
        std::cerr << failures << " debug tools test(s) failed\n";
        return 1;
    }
    std::cout << "Debug tools tests passed\n";
    return 0;
}
