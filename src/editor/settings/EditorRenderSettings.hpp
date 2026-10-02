#pragma once

#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct EditorRenderSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Owns the render settings the editor requests and where they persist: the
// project file when a project was loaded from one, otherwise a standalone
// render-settings document in the editor's settings directory. The renderer
// only ever receives the capability-validated form (see apply).
class EditorRenderSettings {
  public:
    EditorRenderSettings(ProjectConfig project, std::filesystem::path standalonePath);

    // Reads the persisted settings; a missing standalone document keeps the
    // engine defaults. On failure the current request is left unchanged.
    EditorRenderSettingsResult load();
    // Writes the request to its persistent home and clears the unsaved flag.
    EditorRenderSettingsResult save();

    const RenderSettings& requested() const {
        return m_requested;
    }

    // Replaces the request; it is applied by the next apply call.
    void request(const RenderSettings& settings);

    // Gives the renderer the request after capability fallbacks and returns
    // the fallbacks that changed it.
    std::vector<std::string> apply(RenderRuntime& rendering) const;

    bool unsaved() const {
        return m_unsaved;
    }

    bool savesToProject() const {
        return !m_project.projectFile.empty();
    }

    // The file save writes: the project file or the standalone document.
    const std::filesystem::path& storagePath() const;

  private:
    ProjectConfig m_project;
    std::filesystem::path m_standalonePath;
    RenderSettings m_requested;
    bool m_unsaved{false};
};
