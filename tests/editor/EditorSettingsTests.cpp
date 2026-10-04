#include "editor/settings/EditorSettings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
    expect(!settings.tools().buildOnSave, "build on save should be off by default");
    settings.tools().buildOnSave = true;
    expect(settings.save().success, "settings should save");

    EditorSettings restored(settingsPath);
    expect(restored.load().success, "saved settings should load");
    // Panel visibility and the layout version moved to workspaces.json; they
    // are read from old files for migration but no longer saved.
    expect(restored.panels() == EditorPanelVisibility{} && restored.layoutVersion() == 0,
           "panel visibility and layout version should not be saved");
    expect(restored.tools() == settings.tools(),
           "build and IDE commands containing '=', and build on save, should round trip");

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

    // Preferences: every key round trips, and the defaults are the designed look.
    EditorSettings defaults(directory / "defaults.ini");
    expect(defaults.load().success && defaults.appearance() == EditorAppearanceSettings{} &&
               defaults.autosave().intervalSeconds == 30.0f && defaults.tools().packageDirectory == "build-release",
           "a missing file should keep the default preferences");
    expect(defaults.appearance().theme.wallpaper && defaults.appearance().theme.frostRadius == 3 &&
               defaults.appearance().theme.accent == ThemeAccent::Mint && defaults.appearance().uiScale == 0,
           "the default theme is Pixel Aero Night as designed, at the display's scale");
    EditorSettings tuned(directory / "tuned.ini");
    tuned.appearance().theme = {.wallpaper = false,
                                .frostRadius = 5,
                                .glassOpacity = 0.6f,
                                .paneOpacity = 0.95f,
                                .accent = ThemeAccent::Violet};
    tuned.appearance().uiScale = 3;
    tuned.autosave().intervalSeconds = 0.0f;
    tuned.tools().packageDirectory = "out/release pkg";
    expect(tuned.save().success, "tuned preferences should save");
    EditorSettings tunedRestored(directory / "tuned.ini");
    expect(tunedRestored.load().success && tunedRestored.appearance() == tuned.appearance() &&
               tunedRestored.autosave() == tuned.autosave() && tunedRestored.tools() == tuned.tools(),
           "theme, UI scale, autosave, and package settings should round trip");

    EditorSettings clamped(directory / "clamped.ini");
    clamped.appearance().theme.frostRadius = 99;
    clamped.appearance().theme.paneOpacity = 0.1f;
    clamped.appearance().uiScale = 12;
    clamped.autosave().intervalSeconds = -4.0f;
    expect(clamped.save().success, "out-of-range preferences should still save");
    EditorSettings clampedRestored(directory / "clamped.ini");
    expect(clampedRestored.load().success &&
               clampedRestored.appearance().theme.frostRadius == ThemeParameters::maximumFrostRadius &&
               clampedRestored.appearance().theme.paneOpacity == ThemeParameters::minimumPaneOpacity &&
               clampedRestored.appearance().uiScale == maximumUiScale &&
               clampedRestored.autosave().intervalSeconds == 0.0f,
           "out-of-range preferences should be stored clamped so they load again");

    for (const char* invalid : {"theme.frost=7\n", "theme.frost=-1\n", "theme.accent=pink\n", "theme.wallpaper=yes\n",
                                "theme.glass_opacity=0.9\n", "theme.panel_opacity=nan\n", "ui.scale=0\n",
                                "ui.scale=1.5\n", "autosave.interval=-1\n", "autosave.interval=4000\n"}) {
        {
            std::ofstream bad(directory / "bad.ini", std::ios::trunc);
            bad << "build.command=kept\n" << invalid;
        }
        EditorSettings badSettings(directory / "bad.ini");
        const EditorSettingsResult result = badSettings.load();
        expect(!result.success && result.error.find("line 2") != std::string::npos &&
                   badSettings.tools() == EditorToolSettings{},
               "an invalid preference should fail with its line and keep the current settings");
    }
    {
        std::ofstream automatic(directory / "auto.ini", std::ios::trunc);
        automatic << "ui.scale=auto\ntheme.accent=sky\nautosave.interval=12.5\n";
    }
    EditorSettings automaticScale(directory / "auto.ini");
    automaticScale.appearance().uiScale = 2;
    expect(automaticScale.load().success && automaticScale.appearance().uiScale == 0 &&
               automaticScale.appearance().theme.accent == ThemeAccent::Sky &&
               automaticScale.autosave().intervalSeconds == 12.5f,
           "ui.scale=auto, accents by name, and fractional intervals should load");

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
