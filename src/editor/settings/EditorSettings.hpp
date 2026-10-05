#pragma once

#include "editor/theme/ThemeParameters.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// The environment variable that points the editor at another settings
// directory (settings.ini, workspaces.json, keymap.json, ...), so automated
// runs such as the smoke tests never touch the user's settings.
inline constexpr const char* editorSettingsDirectoryVariable = "ZENITH_EDITOR_SETTINGS_DIR";

// The editor's settings directory: utf8Override (the variable's value) when
// it is non-empty, otherwise userDirectory.
std::filesystem::path resolveEditorSettingsDirectory(std::string_view utf8Override,
                                                     std::filesystem::path userDirectory);

// Panel visibility as editors before workspaces.json stored it in
// settings.ini (panel.* keys). Read only, to migrate it into the Layout
// workspace; new panels are area types and need no flag here.
struct EditorPanelVisibility {
    bool viewport{true};
    bool hierarchy{true};
    bool inspector{true};
    bool diagnostics{true};
    bool renderStatistics{true};
    bool assetBrowser{true};
    bool graphics{true};
    bool build{true};
    bool properties{true};
    bool info{true};

    auto operator<=>(const EditorPanelVisibility&) const = default;
};

// The debugger command a new settings file starts with: Visual Studio's
// just-in-time debugger on Windows, none elsewhere.
#ifdef _WIN32
inline constexpr const char* defaultDebugCommand = "vsjitdebugger.exe -p {pid}";
#else
inline constexpr const char* defaultDebugCommand = "";
#endif

// External tools the editor runs. Each value is stored verbatim on one line,
// so it may contain '=' but not line breaks.
struct EditorToolSettings {
    // Shell command the Build panel runs.
    std::string buildCommand{"cmake --build build --config Debug"};
    // Working directory for the build; empty means the editor's working
    // directory. Packaging also runs from here: it must hold the engine's
    // CMakeLists.txt.
    std::string buildDirectory;
    // Command template that opens a file at a line in the IDE. Placeholders:
    // {file}, {line}, {column} (see expandExternalEditorCommand).
    std::string ideCommand{R"(code -g "{file}:{line}:{column}")"};
    // Command template that attaches a debugger to the editor (debug.attach),
    // whose game module code then stops at breakpoints. Placeholder: {pid}.
    // Empty where no default debugger is known.
    std::string debugCommand{defaultDebugCommand};
    // The release build tree Build and Package configures and builds,
    // relative to the build directory. Kept apart from the tree the Build
    // panel uses so packaging never replaces the editor's own build.
    std::string packageDirectory{"build-release"};
    // Whether saving a source file in the project's src folder starts a
    // build (build.on_save), so gameplay code reloads as it is saved; only
    // the project's own build is watched. On by default since settings
    // version 2, which turns it on once in older settings files.
    bool buildOnSave{true};

    auto operator<=>(const EditorToolSettings&) const = default;
};

// How the editor looks: the theme tuning and its size.
struct EditorAppearanceSettings {
    ThemeParameters theme;
    // Screen pixels per art pixel: 0 follows the display's content scale,
    // 1 to maximumUiScale overrides it.
    uint32_t uiScale{0};
    // Whether pie keys (Z, `, .) open pie menus or the ordinary searchable
    // menus (ui.pie_menus).
    bool pieMenus{false};

    bool operator==(const EditorAppearanceSettings&) const = default;
};

struct EditorAutosaveSettings {
    static constexpr float maximumIntervalSeconds = 3600.0f;

    // Seconds a scene stays unsaved before the recovery copy is written;
    // 0 turns autosave off.
    float intervalSeconds{30.0f};

    bool operator==(const EditorAutosaveSettings&) const = default;
};

struct EditorSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Persists editor policy separately from scene data and engine configuration
// in settings.ini (key=value lines). Workspaces and the panel layout are
// saved in workspaces.json instead.
class EditorSettings {
  public:
    explicit EditorSettings(std::filesystem::path settingsPath);

    // Reads the file; a missing file keeps the defaults. An unknown key is
    // ignored, but a known key with an invalid value fails the whole load
    // with its line number and leaves the current settings unchanged.
    EditorSettingsResult load();
    EditorSettingsResult save() const;

    // The legacy panel.* flags read from the file (defaults when absent).
    // They are not saved: workspaces.json replaces them.
    EditorPanelVisibility& panels();
    const EditorPanelVisibility& panels() const;
    EditorToolSettings& tools();
    const EditorToolSettings& tools() const;
    EditorAppearanceSettings& appearance();
    const EditorAppearanceSettings& appearance() const;
    EditorAutosaveSettings& autosave();
    const EditorAutosaveSettings& autosave() const;
    // The legacy layout.version read from the file (0 when absent), which
    // says whether layout.ini is recent enough to migrate. Not saved.
    uint32_t layoutVersion() const;
    void setLayoutVersion(uint32_t version);
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
    EditorPanelVisibility m_panels;
    EditorToolSettings m_tools;
    EditorAppearanceSettings m_appearance;
    EditorAutosaveSettings m_autosave;
    uint32_t m_layoutVersion{0};
};
