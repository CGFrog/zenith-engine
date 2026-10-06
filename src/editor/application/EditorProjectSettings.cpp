// EditorSession's Project Settings workflow: opening the window on the
// project file's values, and saving and applying what it returns.

#include "EditorSession.hpp"

#include <utility>

void EditorSession::openProjectSettings(ApplicationContext&, std::string_view tab) {
    ProjectSettingsValues values;
    if (const ProjectConfigResult loaded = loadProjectSettingsValues(m_projectFile, values); !loaded) {
        m_diagnostics.reportError("Could not open Project Settings: " + loaded.error);
        return;
    }
    const GameModule* module = m_modules.host().module();
    m_projectSettings.open(std::move(values), module != nullptr ? *module->inputActions() : InputActionMap{});
    if (const std::optional<ProjectSettingsTab> shown = projectSettingsTab(tab)) {
        m_projectSettings.setTab(*shown);
    }
}

void EditorSession::drawProjectSettings(ApplicationContext& context) {
    // Physics layers change the bodies of the scene being edited; the
    // running scene keeps the layers it started with.
    const bool editing = m_playMode.mode() == EditorMode::Edit;
    const std::optional<ProjectSettingsValues> applied =
        m_projectSettings.draw(editing, editing ? std::string{} : "Stop to apply");
    if (applied) {
        m_projectSettings.finish(applyProjectSettings(context, *applied));
    }
}

std::string EditorSession::applyProjectSettings(ApplicationContext& context, const ProjectSettingsValues& values) {
    if (const ProjectConfigResult saved = saveProjectSettingsValues(m_projectFile, values); !saved) {
        return saved.error;
    }
    if (values.layers != m_physicsLayers) {
        // setLayers needs a world without bodies; they are rebuilt from the
        // scene, as after a module reload.
        context.physics.clear();
        if (const PhysicsResult result = context.physics.setLayers(values.layers); !result) {
            return "Saved, but the physics layers could not be applied: " + result.error;
        }
        m_physicsLayers = values.layers;
    }
    m_projectNames = {.physicsLayers = values.layers.names, .tags = values.tags};
    // The next navigation.bake and Play use them.
    m_navigationSettings = values.navigation;
    m_modules.host().setProjectInputActions(values.inputActions);
    return {};
}
