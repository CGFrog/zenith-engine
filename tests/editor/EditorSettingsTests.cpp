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
    settings.setLayoutVersion(2);
    expect(settings.save().success, "settings should save");

    EditorSettings restored(settingsPath);
    expect(restored.load().success, "saved settings should load");
    expect(restored.panels() == settings.panels(), "panel visibility should round trip");
    expect(restored.layoutVersion() == 2, "layout version should round trip");

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
