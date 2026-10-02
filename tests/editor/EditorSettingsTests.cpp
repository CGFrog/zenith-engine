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
    settings.setLayoutVersion(2);
    expect(settings.tools().ideCommand == R"(code -g "{file}:{line}:{column}")",
           "the IDE command should default to VS Code at the location");
    settings.tools().buildCommand = R"(cmake --build build --config Release -- /p:Flag=1 "x=y")";
    settings.tools().buildDirectory = "C:/My Projects/game";
    settings.tools().ideCommand = R"(clion --line {line} "{file}")";
    expect(settings.save().success, "settings should save");

    EditorSettings restored(settingsPath);
    expect(restored.load().success, "saved settings should load");
    expect(restored.panels() == settings.panels(), "panel visibility should round trip");
    expect(restored.layoutVersion() == 2, "layout version should round trip");
    expect(restored.tools() == settings.tools(), "build and IDE commands containing '=' should round trip");
    expect(!restored.panels().build, "build panel visibility should round trip");

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

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
