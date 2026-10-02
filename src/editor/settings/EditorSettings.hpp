#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct EditorPanelVisibility {
    bool viewport{true};
    bool hierarchy{true};
    bool inspector{true};
    bool diagnostics{true};
    bool renderStatistics{true};
    bool assetBrowser{true};
    bool graphics{true};
    bool build{true};

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
class EditorSettings {
  public:
    explicit EditorSettings(std::filesystem::path settingsPath);

    EditorSettingsResult load();
    EditorSettingsResult save() const;

    EditorPanelVisibility& panels();
    const EditorPanelVisibility& panels() const;
    EditorToolSettings& tools();
    const EditorToolSettings& tools() const;
    uint32_t layoutVersion() const;
    void setLayoutVersion(uint32_t version);
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
    EditorPanelVisibility m_panels;
    EditorToolSettings m_tools;
    uint32_t m_layoutVersion{0};
};
