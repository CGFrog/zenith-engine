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

    auto operator<=>(const EditorPanelVisibility&) const = default;
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
    uint32_t layoutVersion() const;
    void setLayoutVersion(uint32_t version);
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
    EditorPanelVisibility m_panels;
    uint32_t m_layoutVersion{0};
};
