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

#include "support/Doctest.hpp"

#include <imgui_internal.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

TEST_SUITE_BEGIN("DebugTools");

namespace {

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

TEST_CASE("DebugTools: debugger command") {
    CHECK_MESSAGE(currentProcessId() != 0, "the editor has a process id");
    const ExternalEditorCommandResult expanded = expandDebuggerCommand(R"("my debugger.exe" -p {pid})", 4242);
    CHECK_MESSAGE((expanded && expanded.arguments.size() == 3 && expanded.arguments[0] == "my debugger.exe" &&
                   expanded.arguments[2] == "4242"),
                  "the debugger command substitutes {pid} inside its arguments");
    const ExternalEditorCommandResult wrong = expandDebuggerCommand("gdb {file}", 1);
    CHECK_MESSAGE((!wrong && contains(wrong.error, "debugger command") && contains(wrong.error, "{pid}")),
                  "the debugger command names its only placeholder when another is used");
    const ExternalEditorCommandResult ide = expandExternalEditorCommand("code {path}", {"a.cpp", 1, 1});
    CHECK_MESSAGE((!ide && contains(ide.error, "use {file}, {line}, or {column}")),
                  "the IDE command's errors are unchanged by the shared expansion");
}

struct DebugFixture : EditorTestFixture {
    EditorToolSettings tools;
    WatchList watchList;

    DebugFixture() {
        // The debugger is cmake itself, so debug.attach launches something
        // real without an installed debugger.
        tools.debugCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo attach {pid}";
        CHECK_MESSAGE((static_cast<bool>(registerDebugOperators(operators, {.tools = tools, .watches = watchList}))),
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

TEST_CASE("DebugTools: debug operators") {
    DebugFixture fixture;
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("debug.attach"))), "debug.attach launches the debugger command");
    fixture.tools.debugCommand = "  ";
    CHECK_MESSAGE(!fixture.operators.find("debug.attach")->canRun(fixture.operatorContext()),
                  "debug.attach is unavailable without a debugger command");

    const SceneEntityId lamp = fixture.lamp();
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"))) &&
         fixture.watchList.fields().size() == 1 && fixture.watchList.fields()[0].label == "Intensity"),
        "debug.watch_field adds a field");
    CHECK_MESSAGE((fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity")).status ==
                       OperatorStatus::Cancelled &&
                   fixture.watchList.fields().size() == 1),
                  "a field is watched once");
    CHECK_MESSAGE(
        fixture.invoke("debug.watch_field", R"({"entity":"lamp","component":"point_light","field":"/x"})").status ==
            OperatorStatus::Failed,
        "a bad entity id fails");
    CHECK_MESSAGE(fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "intensity")).status ==
                      OperatorStatus::Failed,
                  "a field that is not a pointer fails");
    CHECK_MESSAGE(fixture.history.size() == 0, "watching records no undo step");
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("debug.unwatch_field", fixture.watchProperties(lamp, "/intensity"))) &&
         fixture.watchList.fields().empty()),
        "debug.unwatch_field removes it");
    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"));
    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/range"));
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("debug.clear_watches")) && fixture.watchList.fields().empty()),
                  "debug.clear_watches removes every field");
}

TEST_CASE("DebugTools: watched values") {
    DebugFixture fixture;
    const SceneEntityId lamp = fixture.lamp();
    const SceneDocumentOptions& options = fixture.document.options();
    const auto value = [&](SceneEntityId entity, std::string component, std::string pointer) {
        return watchedValueText(fixture.scene, fixture.assets, options,
                                {.entity = entity, .component = std::move(component), .pointer = std::move(pointer)});
    };
    CHECK_MESSAGE(value(lamp, "point_light", "/intensity") == "3.5", "a watched field shows its value as JSON");
    CHECK_MESSAGE(value(SceneEntityId{987654321}, "point_light", "/intensity") == "(no entity)",
                  "a missing entity is named");
    CHECK_MESSAGE(value(lamp, "spot_light", "/intensity") == "(no component)", "a missing component is named");
    CHECK_MESSAGE((value(lamp, "point_light", "/no_such_field") == "(no field)" &&
                   value(lamp, "point_light", "~bad") == "(no field)"),
                  "a missing or malformed field is named");
}

TEST_CASE("DebugTools: properties panel watches") {
    DebugFixture fixture;
    const SceneEntityId lamp = fixture.lamp();
    fixture.selection.selectOnly(lamp);
    const testsupport::TempDirectory root("debug-tools-render");
    RenderSettingsController renderSettings{EditorRenderSettings(ProjectConfig{}, root / "render.json")};
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
    CHECK_MESSAGE(!contains(drawLogged(), "Watched"), "the Watched section is hidden while nothing is watched");

    fixture.invoke("debug.watch_field", fixture.watchProperties(lamp, "/intensity"));
    std::string text = drawLogged();
    CHECK_MESSAGE((contains(text, "Watched") && contains(text, "Lamp: Intensity") && contains(text, "3.5")),
                  "the Watched section shows the entity, the field, and its value");
    fixture.scene.registry().get<PointLightComponent>(fixture.scene.findEntity(lamp)).intensity = 8.0f;
    CHECK_MESSAGE(contains(drawLogged(), "8.0"), "the value follows the scene every frame");

    fixture.scene.destroyEntity(fixture.scene.findEntity(lamp));
    text = drawLogged();
    CHECK_MESSAGE((contains(text, "(missing): Intensity") && contains(text, "(no entity)")),
                  "a watch on a destroyed entity stays, saying so");
}
} // namespace

// Entity ids are per scene: the watch operators edit the watch list of the
// document they run on, so a prefab window's watches never reach the level's.
TEST_CASE("DebugTools: watches per document") {
    EditorTestFixture fixture;
    EditorToolSettings tools;
    WatchList levelWatches;
    WatchList windowWatches;
    Scene windowScene;
    EditorDocument windowDocument(fixture.document.options().assetRoot);
    CommandHistory windowHistory;
    Selection windowSelection;
    const auto documentWatches = [&](const OperatorContext& context) -> WatchList* {
        return &context.editing.document() == &windowDocument ? &windowWatches : nullptr;
    };
    CHECK_MESSAGE(
        (static_cast<bool>(registerDebugOperators(
            fixture.operators, {.tools = tools, .watches = levelWatches, .documentWatches = documentWatches}))),
        "per document: the debug operators register");
    const std::string properties = R"({"entity":"1","component":"point_light","field":"/intensity"})";
    OperatorContext window{.editing = {windowScene, fixture.assets, windowDocument, windowHistory, fixture.diagnostics},
                           .selection = windowSelection,
                           .mode = EditorMode::Edit};
    CHECK_MESSAGE((static_cast<bool>(fixture.runner.invokeWithJson("debug.watch_field", window, properties)) &&
                   windowWatches.fields().size() == 1 && levelWatches.fields().empty()),
                  "per document: a watch added in a window goes to the window's list");
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("debug.watch_field", properties)) && levelWatches.fields().size() == 1),
        "per document: the same field watched in the level is the level's own watch");
    CHECK_MESSAGE((static_cast<bool>(fixture.runner.invokeWithJson("debug.clear_watches", window, "{}")) &&
                   windowWatches.fields().empty() && levelWatches.fields().size() == 1),
                  "per document: clearing in a window leaves the level's watches");
}

TEST_SUITE_END();
