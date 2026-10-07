// The editor's settings file: tool commands and preferences round trip,
// legacy and version 1 files migrate, invalid values fail with their line,
// and the settings directory override.

#include "editor/settings/EditorSettings.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>

using testsupport::TempDirectory;
using testsupport::writeText;

TEST_SUITE_BEGIN("EditorSettings");

TEST_CASE("EditorSettings: build and IDE commands round trip without panel visibility") {
    const TempDirectory directory("editor-settings-round-trip");
    const std::filesystem::path settingsPath = directory / "settings.ini";
    EditorSettings settings(settingsPath);
    CHECK_MESSAGE(settings.load().success, "missing settings should use defaults");
    settings.panels().hierarchy = false;
    settings.panels().diagnostics = false;
    settings.panels().build = false;
    settings.panels().properties = false;
    settings.setLayoutVersion(2);
    CHECK_MESSAGE(settings.tools().ideCommand == R"(code -g "{file}:{line}:{column}")",
                  "the IDE command should default to VS Code at the location");
    settings.tools().buildCommand = R"(cmake --build build --config Release -- /p:Flag=1 "x=y")";
    settings.tools().buildDirectory = "C:/My Projects/game";
    settings.tools().ideCommand = R"(clion --line {line} "{file}")";
    CHECK_MESSAGE(settings.tools().buildOnSave, "build on save should be on by default");
    settings.tools().buildOnSave = false;
    CHECK_MESSAGE(settings.tools().debugCommand == std::string(defaultDebugCommand),
                  "the debugger command should start as the platform default");
    settings.tools().debugCommand = "gdb -p {pid}";
    CHECK_MESSAGE(settings.save().success, "settings should save");

    EditorSettings restored(settingsPath);
    CHECK_MESSAGE(restored.load().success, "saved settings should load");
    // Panel visibility and the layout version moved to workspaces.json; they
    // are read from old files for migration but no longer saved.
    CHECK_MESSAGE((restored.panels() == EditorPanelVisibility{} && restored.layoutVersion() == 0),
                  "panel visibility and layout version should not be saved");
    CHECK_MESSAGE(restored.tools() == settings.tools(),
                  "build and IDE commands containing '=', and build on save, should round trip");
}

TEST_CASE("EditorSettings: legacy panel flags and layout version are read for migration") {
    const TempDirectory directory("editor-settings-legacy");
    writeText(directory / "legacy.ini", "layout.version=4\npanel.build=false\npanel.hierarchy=false\n");
    EditorSettings legacySettings(directory / "legacy.ini");
    CHECK_MESSAGE((legacySettings.load().success && legacySettings.layoutVersion() == 4 &&
                   !legacySettings.panels().build && !legacySettings.panels().hierarchy &&
                   legacySettings.panels().viewport),
                  "legacy panel flags and layout version should still be read for migration");
}

// Version 1 files wrote build.on_save=false whether or not it was chosen;
// build on save became the default in version 2, so it turns on once.
TEST_CASE("EditorSettings: a version 1 file turns build on save on once") {
    const TempDirectory directory("editor-settings-version-one");
    writeText(directory / "version1.ini", "# Zenith editor settings\nbuild.on_save=false\n");
    EditorSettings upgraded(directory / "version1.ini");
    CHECK_MESSAGE((upgraded.load().success && upgraded.tools().buildOnSave),
                  "a settings file from before version 2 turns build on save on");
    CHECK_MESSAGE(upgraded.save().success, "upgraded settings should save");
    upgraded.tools().buildOnSave = false;
    CHECK_MESSAGE(upgraded.save().success, "a chosen build on save should save");
    EditorSettings chosen(directory / "version1.ini");
    CHECK_MESSAGE((chosen.load().success && !chosen.tools().buildOnSave),
                  "build on save turned off after the upgrade stays off");
}

TEST_CASE("EditorSettings: commands with line breaks are rejected on save") {
    const TempDirectory directory("editor-settings-multiline");
    EditorSettings multiline(directory / "multiline.ini");
    multiline.tools().buildCommand = "first\nsecond";
    CHECK_MESSAGE(!multiline.save().success, "commands with line breaks should be rejected on save");
}

TEST_CASE("EditorSettings: empty values load and unspecified tools keep defaults") {
    const TempDirectory directory("editor-settings-empty-values");
    const std::filesystem::path settingsPath = directory / "settings.ini";
    writeText(settingsPath, "# comment\nbuild.command=\nbuild.directory=\n");
    EditorSettings cleared(settingsPath);
    CHECK_MESSAGE((cleared.load().success && cleared.tools().buildCommand.empty() &&
                   cleared.tools().ideCommand == EditorToolSettings{}.ideCommand),
                  "empty values should load and unspecified tools should keep defaults");
}

TEST_CASE("EditorSettings: a malformed file fails and keeps the current settings") {
    const TempDirectory directory("editor-settings-malformed");
    const std::filesystem::path settingsPath = directory / "settings.ini";
    // Settings loaded from a saved file, as the session holds them.
    EditorSettings saved(settingsPath);
    saved.save();
    EditorSettings restored(settingsPath);
    restored.load();

    writeText(settingsPath, "panel.hierarchy=maybe\n");
    const EditorPanelVisibility before = restored.panels();
    CHECK_MESSAGE(!restored.load().success, "malformed settings should fail descriptively");
    CHECK_MESSAGE(restored.panels() == before, "failed load should preserve current settings");
}

// Preferences: every key round trips, and the defaults are the designed look.
TEST_CASE("EditorSettings: a missing file keeps the designed default preferences") {
    const TempDirectory directory("editor-settings-default-preferences");
    EditorSettings defaults(directory / "defaults.ini");
    CHECK_MESSAGE((defaults.load().success && defaults.appearance() == EditorAppearanceSettings{} &&
                   defaults.autosave().intervalSeconds == 30.0f &&
                   defaults.tools().packageDirectory == "build-release"),
                  "a missing file should keep the default preferences");
    CHECK_MESSAGE((defaults.appearance().theme.accent == ThemeAccent::Blue && defaults.appearance().uiScale == 0),
                  "the default theme is 8-bit Blender as designed, at the display's scale");
}

TEST_CASE("EditorSettings: theme and UI scale and autosave and package settings round trip") {
    const TempDirectory directory("editor-settings-preferences");
    EditorSettings tuned(directory / "tuned.ini");
    tuned.appearance().theme = {.accent = ThemeAccent::Violet};
    tuned.appearance().uiScale = 3;
    tuned.appearance().pieMenus = true;
    tuned.autosave().intervalSeconds = 0.0f;
    tuned.tools().packageDirectory = "out/release pkg";
    CHECK_MESSAGE(tuned.save().success, "tuned preferences should save");
    EditorSettings tunedRestored(directory / "tuned.ini");
    CHECK_MESSAGE((tunedRestored.load().success && tunedRestored.appearance() == tuned.appearance() &&
                   tunedRestored.autosave() == tuned.autosave() && tunedRestored.tools() == tuned.tools()),
                  "theme, UI scale, autosave, and package settings should round trip");
}

TEST_CASE("EditorSettings: out-of-range preferences are stored clamped") {
    const TempDirectory directory("editor-settings-clamped");
    EditorSettings clamped(directory / "clamped.ini");
    clamped.appearance().theme.accent = static_cast<ThemeAccent>(99);
    clamped.appearance().uiScale = 12;
    clamped.autosave().intervalSeconds = -4.0f;
    CHECK_MESSAGE(clamped.save().success, "out-of-range preferences should still save");
    EditorSettings clampedRestored(directory / "clamped.ini");
    CHECK_MESSAGE((clampedRestored.load().success && clampedRestored.appearance().theme.accent == ThemeAccent::Blue &&
                   clampedRestored.appearance().uiScale == maximumUiScale &&
                   clampedRestored.autosave().intervalSeconds == 0.0f),
                  "out-of-range preferences should be stored clamped so they load again");
}

TEST_CASE("EditorSettings: an invalid preference fails with its line") {
    const TempDirectory directory("editor-settings-invalid");
    for (const char* invalid : {"theme.accent=pink\n", "ui.scale=0\n", "ui.scale=1.5\n", "autosave.interval=-1\n",
                                "autosave.interval=4000\n"}) {
        CAPTURE(invalid);
        writeText(directory / "bad.ini", std::string("build.command=kept\n") + invalid);
        EditorSettings badSettings(directory / "bad.ini");
        const EditorSettingsResult result = badSettings.load();
        CHECK_MESSAGE((!result.success && result.error.find("line 2") != std::string::npos &&
                       badSettings.tools() == EditorToolSettings{}),
                      "an invalid preference should fail with its line and keep the current settings");
    }
}

TEST_CASE("EditorSettings: an automatic scale and named accents and fractional intervals load") {
    const TempDirectory directory("editor-settings-automatic");
    writeText(directory / "auto.ini", "ui.scale=auto\ntheme.accent=teal\nautosave.interval=12.5\n");
    EditorSettings automaticScale(directory / "auto.ini");
    automaticScale.appearance().uiScale = 2;
    CHECK_MESSAGE((automaticScale.load().success && automaticScale.appearance().uiScale == 0 &&
                   automaticScale.appearance().theme.accent == ThemeAccent::Teal &&
                   automaticScale.autosave().intervalSeconds == 12.5f),
                  "ui.scale=auto, accents by name, and fractional intervals should load");
}

TEST_CASE("EditorSettings: a Pixel Aero Night settings file loads") {
    const TempDirectory directory("editor-settings-aero");
    writeText(directory / "aero.ini",
              "theme.wallpaper=false\ntheme.frost=5\ntheme.glass_opacity=0.6\ntheme.panel_opacity=0.95\n"
              "theme.accent=mint\nbuild.command=kept\n");
    EditorSettings aeroSettings(directory / "aero.ini");
    CHECK_MESSAGE((aeroSettings.load().success && aeroSettings.appearance().theme.accent == ThemeAccent::Teal &&
                   aeroSettings.tools().buildCommand == "kept"),
                  "a Pixel Aero Night settings file loads, its glass keys skipped and its accent mapped");
}

// ZENITH_EDITOR_SETTINGS_DIR replaces the user's directory when set.
TEST_CASE("EditorSettings: the settings directory override replaces the user directory") {
    const TempDirectory directory("editor-settings-directory");
    const std::filesystem::path user = directory / "user";
    CHECK_MESSAGE(resolveEditorSettingsDirectory("", user) == user,
                  "without an override the user directory should be used");
    CHECK_MESSAGE(resolveEditorSettingsDirectory("C:/ci/editor-settings", user) ==
                      std::filesystem::path("C:/ci/editor-settings"),
                  "an override should replace the user directory");
    CHECK_MESSAGE(resolveEditorSettingsDirectory("D:/caf\xC3\xA9", user) == std::filesystem::path(u8"D:/caf\u00e9"),
                  "an override should be read as UTF-8");
}

TEST_SUITE_END();
