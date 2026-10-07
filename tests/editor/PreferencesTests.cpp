#include "HeadlessImGui.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PreferencesOperators.hpp"
#include "editor/panels/PreferencesWindow.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

bool hasProblem(const std::vector<PreferenceProblem>& problems, PreferenceSection section, const std::string& text) {
    return std::ranges::any_of(problems, [&](const PreferenceProblem& problem) {
        return problem.section == section && problem.message.find(text) != std::string::npos;
    });
}

ImGuiID itemInPreferences(const char* section, const char* label) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Preferences test");
    return window == nullptr ? 0 : HeadlessImGuiContext::itemId(*window, section, label);
}

} // namespace

TEST_SUITE_BEGIN("Preferences");

TEST_CASE("Preferences: validation") {
    const testsupport::TempDirectory root("preferences-validation");
    const std::filesystem::path& directory = root.path();
    EditorPreferenceValues values;
    CHECK_MESSAGE(validatePreferences(values, directory).empty(), "the defaults are valid");

    values.tools.buildCommand = "   ";
    values.tools.ideCommand = "code {path}";
    values.tools.buildDirectory = (directory / "missing").string();
    values.tools.packageDirectory = "";
    std::vector<PreferenceProblem> problems = validatePreferences(values, directory);
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::Build, "build command is empty"),
                  "an empty build command is reported");
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::ExternalIde, "invalid"),
                  "an unknown IDE placeholder is reported");
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::Build, "does not exist"),
                  "a missing build directory is reported");
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::Build, "package build directory is empty"),
                  "an empty package directory is reported");

    values = {};
    values.tools.ideCommand = "";
    values.tools.packageDirectory = ".";
    problems = validatePreferences(values, directory);
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::ExternalIde, "empty"), "an empty IDE command is reported");
    CHECK_MESSAGE(hasProblem(problems, PreferenceSection::Build, "cannot be the build directory"),
                  "packaging into the build directory itself is reported");

    values = {};
    values.autosave.intervalSeconds = -1.0f;
    values.appearance.theme.accent = static_cast<ThemeAccent>(99);
    problems = validatePreferences(values, directory);
    CHECK_MESSAGE((hasProblem(problems, PreferenceSection::Saving, "autosave") &&
                   hasProblem(problems, PreferenceSection::Interface, "range")),
                  "out-of-range autosave and theme values are reported");

    EditorPreferenceValues edited;
    edited.appearance.theme.accent = ThemeAccent::Green;
    edited.appearance.uiScale = 2;
    edited.tools.buildCommand = "make";
    edited.tools.ideCommand = "vim +{line} {file}";
    edited.autosave.intervalSeconds = 0.0f;
    const EditorPreferenceValues interfaceReset = resetPreferenceSection(edited, PreferenceSection::Interface);
    CHECK_MESSAGE(
        (interfaceReset.appearance == EditorAppearanceSettings{} && interfaceReset.tools.buildCommand == "make"),
        "resetting a section restores only that section");
    const EditorPreferenceValues buildReset = resetPreferenceSection(edited, PreferenceSection::Build);
    CHECK_MESSAGE((buildReset.tools.buildCommand == EditorToolSettings{}.buildCommand &&
                   buildReset.tools.ideCommand == "vim +{line} {file}" && buildReset.autosave.intervalSeconds == 0.0f),
                  "resetting Build keeps the IDE command and autosave");
    CHECK_MESSAGE(resetPreferenceSection(edited, PreferenceSection::Saving).autosave == EditorAutosaveSettings{},
                  "resetting Saving restores the 30 second autosave");
}

TEST_CASE("Preferences: operator opens window") {
    const testsupport::TempDirectory root("preferences-operator");
    Scene scene;
    AssetManager assets;
    EditorDocument document{root.path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};
    PreferencesWindow preferences;
    CHECK_MESSAGE(registerPreferencesOperators(registry, preferences).success, "wm.preferences registers");
    OperatorContext context{.editing = SceneEditing(scene, assets, document, history, diagnostics),
                            .selection = selection};
    CHECK_MESSAGE(!preferences.isOpen(), "the window starts closed");
    CHECK_MESSAGE(static_cast<bool>(runner.invokeWithJson("wm.preferences", context, "")),
                  "wm.preferences runs headless");
    CHECK_MESSAGE(preferences.isOpen(), "wm.preferences opens the Preferences window");
}

TEST_CASE("Preferences: window applies valid edits") {
    const testsupport::TempDirectory root("preferences-window");
    const std::filesystem::path& directory = root.path();
    HeadlessImGuiContext imgui;
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
    applyEditorColors(ImGui::GetStyle());
    EditorSettings settings(directory / "settings.ini");
    PreferencesWindow preferences;
    bool keymapOpened = false;
    const PreferencesContext context{.keymapPath = directory / "keymap.json",
                                     .displayContentScale = 2.0f,
                                     .openKeymapEditor = [&keymapOpened] { keymapOpened = true; },
                                     .ideTestFile = directory / "settings.ini",
                                     .workingDirectory = directory};
    const auto draw = [&] {
        HeadlessImGuiFrame frame("Preferences test");
        return preferences.drawContents(settings, context);
    };
    CHECK_MESSAGE(!draw(), "drawing without edits changes nothing");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "Pie menus"));
    CHECK_MESSAGE((draw() && settings.appearance().pieMenus), "ticking Pie menus applies at once");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "3x"));
    CHECK_MESSAGE((draw() && settings.appearance().uiScale == 3), "choosing a UI scale override applies at once");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("keymap", "Edit Keymap..."));
    draw();
    CHECK_MESSAGE(keymapOpened, "the Keymap section opens the keymap editor");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("saving", "Autosave a recovery copy"));
    CHECK_MESSAGE((draw() && settings.autosave().intervalSeconds == 0.0f), "unticking autosave turns it off");
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("saving", "Autosave a recovery copy"));
    CHECK_MESSAGE((draw() && settings.autosave().intervalSeconds == 30.0f), "ticking autosave restores its interval");

    // Clearing the build command is a problem: Build keeps its last valid
    // values while other sections still apply.
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("build", "Build command"));
    draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false);
    draw();
    CHECK_MESSAGE(preferences.draft().tools.buildCommand.empty(), "the build command field was cleared");
    CHECK_MESSAGE(settings.tools().buildCommand == EditorToolSettings{}.buildCommand,
                  "an empty build command is not stored");
    CHECK_MESSAGE(std::ranges::any_of(
                      preferences.problems(),
                      [](const PreferenceProblem& problem) { return problem.section == PreferenceSection::Build; }),
                  "the Build section shows its problem");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "Reset to Defaults"));
    CHECK_MESSAGE((draw() && settings.appearance() == EditorAppearanceSettings{} &&
                   settings.tools().buildCommand == EditorToolSettings{}.buildCommand),
                  "resetting Interface applies while Build stays at its last valid value");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("build", "Reset to Defaults"));
    draw();
    CHECK_MESSAGE(preferences.problems().empty(), "resetting Build clears its problem");

    // The Test button launches the IDE command; cmake stands in for an IDE.
    settings.tools().ideCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo {file}:{line}";
    draw();
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("ide", "Test"));
    draw();
    CHECK_MESSAGE(preferences.ideTestResult().find("Started") != std::string::npos, "the Test button starts the IDE");

    CHECK_MESSAGE(settings.save().success, "the edited settings save");
    EditorSettings reloaded(directory / "settings.ini");
    CHECK_MESSAGE((reloaded.load().success && reloaded.appearance() == settings.appearance() &&
                   reloaded.tools() == settings.tools()),
                  "preferences persist through settings.ini");

    preferences.open();
    {
        // The window itself, with every section open.
        ImGui::NewFrame();
        preferences.draw(settings, context);
        ImGui::Render();
    }
    CHECK_MESSAGE(preferences.isOpen(), "the open window draws");
}

TEST_SUITE_END();
