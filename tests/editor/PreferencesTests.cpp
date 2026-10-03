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

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool hasProblem(const std::vector<PreferenceProblem>& problems, PreferenceSection section, const std::string& text) {
    return std::ranges::any_of(problems, [&](const PreferenceProblem& problem) {
        return problem.section == section && problem.message.find(text) != std::string::npos;
    });
}

void testValidation(const std::filesystem::path& directory) {
    EditorPreferenceValues values;
    expect(validatePreferences(values, directory).empty(), "the defaults are valid");

    values.tools.buildCommand = "   ";
    values.tools.ideCommand = "code {path}";
    values.tools.buildDirectory = (directory / "missing").string();
    values.tools.packageDirectory = "";
    std::vector<PreferenceProblem> problems = validatePreferences(values, directory);
    expect(hasProblem(problems, PreferenceSection::Build, "build command is empty"),
           "an empty build command is reported");
    expect(hasProblem(problems, PreferenceSection::ExternalIde, "invalid"), "an unknown IDE placeholder is reported");
    expect(hasProblem(problems, PreferenceSection::Build, "does not exist"), "a missing build directory is reported");
    expect(hasProblem(problems, PreferenceSection::Build, "package build directory is empty"),
           "an empty package directory is reported");

    values = {};
    values.tools.ideCommand = "";
    values.tools.packageDirectory = ".";
    problems = validatePreferences(values, directory);
    expect(hasProblem(problems, PreferenceSection::ExternalIde, "empty"), "an empty IDE command is reported");
    expect(hasProblem(problems, PreferenceSection::Build, "cannot be the build directory"),
           "packaging into the build directory itself is reported");

    values = {};
    values.autosave.intervalSeconds = -1.0f;
    values.appearance.theme.paneOpacity = 0.2f;
    problems = validatePreferences(values, directory);
    expect(hasProblem(problems, PreferenceSection::Saving, "autosave") &&
               hasProblem(problems, PreferenceSection::Interface, "range"),
           "out-of-range autosave and theme values are reported");

    EditorPreferenceValues edited;
    edited.appearance.theme.accent = ThemeAccent::Lime;
    edited.appearance.uiScale = 2;
    edited.tools.buildCommand = "make";
    edited.tools.ideCommand = "vim +{line} {file}";
    edited.autosave.intervalSeconds = 0.0f;
    const EditorPreferenceValues interfaceReset = resetPreferenceSection(edited, PreferenceSection::Interface);
    expect(interfaceReset.appearance == EditorAppearanceSettings{} && interfaceReset.tools.buildCommand == "make",
           "resetting a section restores only that section");
    const EditorPreferenceValues buildReset = resetPreferenceSection(edited, PreferenceSection::Build);
    expect(buildReset.tools.buildCommand == EditorToolSettings{}.buildCommand &&
               buildReset.tools.ideCommand == "vim +{line} {file}" && buildReset.autosave.intervalSeconds == 0.0f,
           "resetting Build keeps the IDE command and autosave");
    expect(resetPreferenceSection(edited, PreferenceSection::Saving).autosave == EditorAutosaveSettings{},
           "resetting Saving restores the 30 second autosave");
}

void testOperatorOpensWindow() {
    Scene scene;
    AssetManager assets;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};
    PreferencesWindow preferences;
    expect(registerPreferencesOperators(registry, preferences).success, "wm.preferences registers");
    OperatorContext context{.editing = SceneEditing(scene, assets, document, history, diagnostics),
                            .selection = selection};
    expect(!preferences.isOpen(), "the window starts closed");
    expect(static_cast<bool>(runner.invokeWithJson("wm.preferences", context, "")), "wm.preferences runs headless");
    expect(preferences.isOpen(), "wm.preferences opens the Preferences window");
}

ImGuiID itemInPreferences(const char* section, const char* label) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Preferences test");
    return window == nullptr ? 0 : HeadlessImGuiContext::itemId(*window, section, label);
}

void testWindowAppliesValidEdits(const std::filesystem::path& directory) {
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
    expect(!draw(), "drawing without edits changes nothing");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "Wallpaper"));
    expect(draw() && !settings.appearance().theme.wallpaper, "unticking Wallpaper applies at once");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "3x"));
    expect(draw() && settings.appearance().uiScale == 3, "choosing a UI scale override applies at once");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("keymap", "Edit Keymap..."));
    draw();
    expect(keymapOpened, "the Keymap section opens the keymap editor");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("saving", "Autosave a recovery copy"));
    expect(draw() && settings.autosave().intervalSeconds == 0.0f, "unticking autosave turns it off");
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("saving", "Autosave a recovery copy"));
    expect(draw() && settings.autosave().intervalSeconds == 30.0f, "ticking autosave restores its interval");

    // Clearing the build command is a problem: Build keeps its last valid
    // values while other sections still apply.
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("build", "Build command"));
    draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false);
    draw();
    expect(preferences.draft().tools.buildCommand.empty(), "the build command field was cleared");
    expect(settings.tools().buildCommand == EditorToolSettings{}.buildCommand, "an empty build command is not stored");
    expect(std::ranges::any_of(
               preferences.problems(),
               [](const PreferenceProblem& problem) { return problem.section == PreferenceSection::Build; }),
           "the Build section shows its problem");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("interface", "Reset to Defaults"));
    expect(draw() && settings.appearance() == EditorAppearanceSettings{} &&
               settings.tools().buildCommand == EditorToolSettings{}.buildCommand,
           "resetting Interface applies while Build stays at its last valid value");

    HeadlessImGuiContext::activateNextFrame(itemInPreferences("build", "Reset to Defaults"));
    draw();
    expect(preferences.problems().empty(), "resetting Build clears its problem");

    // The Test button launches the IDE command; cmake stands in for an IDE.
    settings.tools().ideCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo {file}:{line}";
    draw();
    HeadlessImGuiContext::activateNextFrame(itemInPreferences("ide", "Test"));
    draw();
    expect(preferences.ideTestResult().find("Started") != std::string::npos, "the Test button starts the IDE");

    expect(settings.save().success, "the edited settings save");
    EditorSettings reloaded(directory / "settings.ini");
    expect(reloaded.load().success && reloaded.appearance() == settings.appearance() &&
               reloaded.tools() == settings.tools(),
           "preferences persist through settings.ini");

    preferences.open();
    {
        // The window itself, with every section open.
        ImGui::NewFrame();
        preferences.draw(settings, context);
        ImGui::Render();
    }
    expect(preferences.isOpen(), "the open window draws");
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_preferences_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    testValidation(directory);
    testOperatorOpensWindow();
    testWindowAppliesValidEdits(directory);
    std::filesystem::remove_all(directory, error);
    if (failures != 0) {
        std::cerr << failures << " preferences test(s) failed\n";
        return 1;
    }
    std::cout << "Preferences tests passed\n";
    return 0;
}
