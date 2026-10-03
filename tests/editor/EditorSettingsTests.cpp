#include "editor/settings/EditorSettings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_editor_settings_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const std::filesystem::path settingsPath = directory / "settings.ini";
    EditorSettings settings(settingsPath);
    expect(settings.load().success, "missing settings should use defaults");
    settings.panels().hierarchy = false;
    settings.panels().diagnostics = false;
    settings.panels().build = false;
    settings.panels().properties = false;
    settings.setLayoutVersion(2);
    expect(settings.tools().ideCommand == R"(code -g "{file}:{line}:{column}")",
           "the IDE command should default to VS Code at the location");
    settings.tools().buildCommand = R"(cmake --build build --config Release -- /p:Flag=1 "x=y")";
    settings.tools().buildDirectory = "C:/My Projects/game";
    settings.tools().ideCommand = R"(clion --line {line} "{file}")";
    expect(settings.save().success, "settings should save");

    EditorSettings restored(settingsPath);
    expect(restored.load().success, "saved settings should load");
    // Panel visibility and the layout version moved to workspaces.json; they
    // are read from old files for migration but no longer saved.
    expect(restored.panels() == EditorPanelVisibility{} && restored.layoutVersion() == 0,
           "panel visibility and layout version should not be saved");
    expect(restored.tools() == settings.tools(), "build and IDE commands containing '=' should round trip");

    {
        std::ofstream legacy(directory / "legacy.ini", std::ios::trunc);
        legacy << "layout.version=4\npanel.build=false\npanel.hierarchy=false\n";
    }
    EditorSettings legacySettings(directory / "legacy.ini");
    expect(legacySettings.load().success && legacySettings.layoutVersion() == 4 && !legacySettings.panels().build &&
               !legacySettings.panels().hierarchy && legacySettings.panels().viewport,
           "legacy panel flags and layout version should still be read for migration");

    EditorSettings multiline(directory / "multiline.ini");
    multiline.tools().buildCommand = "first\nsecond";
    expect(!multiline.save().success, "commands with line breaks should be rejected on save");

    {
        std::ofstream emptyValues(settingsPath, std::ios::trunc);
        emptyValues << "# comment\nbuild.command=\nbuild.directory=\n";
    }
    EditorSettings cleared(settingsPath);
    expect(cleared.load().success && cleared.tools().buildCommand.empty() &&
               cleared.tools().ideCommand == EditorToolSettings{}.ideCommand,
           "empty values should load and unspecified tools should keep defaults");

    {
        std::ofstream malformed(settingsPath, std::ios::trunc);
        malformed << "panel.hierarchy=maybe\n";
    }
    const EditorPanelVisibility before = restored.panels();
    expect(!restored.load().success, "malformed settings should fail descriptively");
    expect(restored.panels() == before, "failed load should preserve current settings");

    // ZENITH_EDITOR_SETTINGS_DIR replaces the user's directory when set.
    const std::filesystem::path user = directory / "user";
    expect(resolveEditorSettingsDirectory("", user) == user, "without an override the user directory should be used");
    expect(resolveEditorSettingsDirectory("C:/ci/editor-settings", user) ==
               std::filesystem::path("C:/ci/editor-settings"),
           "an override should replace the user directory");
    expect(resolveEditorSettingsDirectory("D:/caf\xC3\xA9", user) == std::filesystem::path(u8"D:/caf\u00e9"),
           "an override should be read as UTF-8");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
