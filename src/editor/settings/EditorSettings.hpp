#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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

// External tools the editor runs. Each value is stored verbatim on one line,
// so it may contain '=' but not line breaks.
struct EditorToolSettings {
    // Shell command the Build panel runs.
    std::string buildCommand{"cmake --build build --config Debug"};
    // Working directory for the build; empty means the editor's working
    // directory.
    std::string buildDirectory;
    // Command template that opens a file at a line in the IDE. Placeholders:
    // {file}, {line}, {column} (see expandExternalEditorCommand).
    std::string ideCommand{R"(code -g "{file}:{line}:{column}")"};

    auto operator<=>(const EditorToolSettings&) const = default;
};

struct EditorSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Persists editor policy separately from scene data and engine configuration.
// Workspaces and the panel layout are saved in workspaces.json instead.
class EditorSettings {
  public:
    explicit EditorSettings(std::filesystem::path settingsPath);

    EditorSettingsResult load();
    EditorSettingsResult save() const;

    // The legacy panel.* flags read from the file (defaults when absent).
    // They are not saved: workspaces.json replaces them.
    EditorPanelVisibility& panels();
    const EditorPanelVisibility& panels() const;
    EditorToolSettings& tools();
    const EditorToolSettings& tools() const;
    // The legacy layout.version read from the file (0 when absent), which
    // says whether layout.ini is recent enough to migrate. Not saved.
    uint32_t layoutVersion() const;
    void setLayoutVersion(uint32_t version);
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
    EditorPanelVisibility m_panels;
    EditorToolSettings m_tools;
    uint32_t m_layoutVersion{0};
};
