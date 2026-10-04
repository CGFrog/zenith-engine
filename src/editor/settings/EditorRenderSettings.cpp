#include "EditorRenderSettings.hpp"

#include <fstream>
#include <sstream>
#include <utility>

EditorRenderSettings::EditorRenderSettings(ProjectConfig project, std::filesystem::path standalonePath)
    : m_project(std::move(project))
    , m_standalonePath(std::move(standalonePath))
    , m_requested(m_project.renderSettings) {}

EditorRenderSettingsResult EditorRenderSettings::load() {
    if (savesToProject()) {
        // Re-read the file so a session created after an earlier save sees it.
        ProjectConfig loaded;
        if (const ProjectConfigResult result = loadProjectConfig(m_project.projectFile, loaded); !result) {
            return {.error = result.error};
        }
        m_project = std::move(loaded);
        m_requested = m_project.renderSettings;
        m_unsaved = false;
        return {.success = true};
    }
    std::error_code error;
    if (!std::filesystem::exists(m_standalonePath, error)) {
        if (error) {
            return {.error = "Cannot inspect render settings: " + error.message()};
        }
        // Nothing saved yet: the request returns to the initial settings.
        m_requested = m_project.renderSettings;
        m_unsaved = false;
        return {.success = true};
    }
    std::ifstream input(m_standalonePath);
    if (!input) {
        return {.error = "Cannot read render settings: " + m_standalonePath.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    RenderSettings loaded;
    if (const RenderConfigurationResult result = deserializeRenderSettings(contents.str(), loaded); !result) {
        return {.error = "Invalid render settings in " + m_standalonePath.string() + ": " + result.error};
    }
    m_requested = loaded;
    m_unsaved = false;
    return {.success = true};
}

EditorRenderSettingsResult EditorRenderSettings::save() {
    if (savesToProject()) {
        // The rest of the file may have changed since (Project Settings saves
        // it too), so only the render settings are written over it.
        ProjectConfig updated;
        if (!loadProjectConfig(m_project.projectFile, updated)) {
            updated = m_project;
        }
        updated.renderSettings = m_requested;
        if (const ProjectConfigResult result = saveProjectConfig(updated.projectFile, updated); !result) {
            return {.error = result.error};
        }
        m_project = std::move(updated);
        m_unsaved = false;
        return {.success = true};
    }
    std::string serialized;
    if (const RenderConfigurationResult result = serializeRenderSettings(m_requested, serialized); !result) {
        return {.error = result.error};
    }
    std::error_code error;
    std::filesystem::create_directories(m_standalonePath.parent_path(), error);
    if (error) {
        return {.error = "Cannot create render settings directory: " + error.message()};
    }
    std::ofstream output(m_standalonePath, std::ios::trunc);
    output << serialized << '\n';
    if (!output) {
        return {.error = "Cannot write render settings: " + m_standalonePath.string()};
    }
    m_unsaved = false;
    return {.success = true};
}

void EditorRenderSettings::request(const RenderSettings& settings) {
    m_requested = settings;
    m_unsaved = true;
}

std::vector<std::string> EditorRenderSettings::apply(RenderRuntime& rendering) const {
    ValidatedRenderSettings validated = validateRenderSettings(m_requested, rendering.capabilities());
    rendering.setSettings(validated.settings);
    return std::move(validated.fallbacks);
}

const std::filesystem::path& EditorRenderSettings::storagePath() const {
    return savesToProject() ? m_project.projectFile : m_standalonePath;
}
