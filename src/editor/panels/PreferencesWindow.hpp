#pragma once

// The Preferences window: interface, keymap, external IDE, saving, build.

#include "editor/application/FolderPicker.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

// Every value the Preferences window edits.
struct EditorPreferenceValues {
    EditorAppearanceSettings appearance;
    EditorAutosaveSettings autosave;
    EditorToolSettings tools;

    bool operator==(const EditorPreferenceValues&) const = default;
};

EditorPreferenceValues preferenceValues(const EditorSettings& settings);
void storePreferenceValues(EditorSettings& settings, const EditorPreferenceValues& values);

enum class PreferenceSection : uint8_t {
    Interface,
    Keymap,
    ExternalIde,
    Saving,
    Build,
};

struct PreferenceProblem {
    PreferenceSection section{PreferenceSection::Interface};
    std::string message;
};

// What is wrong with values, by section: an empty build or IDE command, an
// IDE command with an unknown placeholder or unbalanced quotes, a build
// directory that does not exist, or a package directory that is empty or the
// build directory itself. Relative directories resolve against
// workingDirectory.
std::vector<PreferenceProblem> validatePreferences(const EditorPreferenceValues& values,
                                                   const std::filesystem::path& workingDirectory);
// values with one section back at its defaults (Keymap has none here; the
// keymap editor resets bindings).
EditorPreferenceValues resetPreferenceSection(EditorPreferenceValues values, PreferenceSection section);

struct PreferencesContext {
    // keymap.json, shown in the Keymap section.
    std::filesystem::path keymapPath;
    // The display's content scale, for the Automatic UI scale.
    float displayContentScale{1.0f};
    // Opens the keymap editor (wm.keymap_editor).
    std::function<void()> openKeymapEditor;
    // The file the IDE command's Test button opens.
    std::filesystem::path ideTestFile;
    // Where relative build directories resolve; the editor's working
    // directory.
    std::filesystem::path workingDirectory;
};

// Edit > Preferences: the theme tuning, UI scale, keymap, external IDE,
// autosave, and build settings. Edits apply live: each change is validated,
// and every section without a problem is stored in the settings at once
// (the caller saves settings.ini and applies the look). A section with a
// problem keeps its last valid values until the problem is fixed.
class PreferencesWindow {
  public:
    void open();
    bool isOpen() const;

    // Draws the window while it is open. Returns true when settings changed.
    bool draw(EditorSettings& settings, const PreferencesContext& context);
    // The window's contents, for tests.
    bool drawContents(EditorSettings& settings, const PreferencesContext& context);

    // The values being edited, including ones not yet valid.
    const EditorPreferenceValues& draft() const;
    std::span<const PreferenceProblem> problems() const;
    // What the IDE command's Test button last did.
    const std::string& ideTestResult() const;

  private:
    // Restarts editing from settings when they changed outside the window
    // (the Build panel edits the same build settings).
    void syncWithSettings(const EditorSettings& settings, const std::filesystem::path& workingDirectory);
    bool commitValidSections(EditorSettings& settings);

    bool drawInterface(const PreferencesContext& context);
    void drawKeymap(const PreferencesContext& context);
    bool drawExternalIde(const PreferencesContext& context);
    bool drawSaving();
    bool drawBuild();
    bool drawPackageDirectoryPicker();
    bool drawSectionFooter(PreferenceSection section);

    bool m_open{false};
    bool m_synced{false};
    EditorPreferenceValues m_committed;
    EditorPreferenceValues m_draft;
    std::vector<PreferenceProblem> m_problems;
    // The interval autosave returns to when it is switched back on.
    float m_enabledAutosaveInterval{EditorAutosaveSettings{}.intervalSeconds};
    std::filesystem::path m_workingDirectory;
    FolderPicker m_buildDirectoryPicker;
    FolderPicker m_packageDirectoryPicker;
    std::string m_ideTestResult;
    bool m_ideTestFailed{false};
};
