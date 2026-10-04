#pragma once

#include "engine/input/InputActions.hpp"
#include "engine/physics/PhysicsLayers.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Every value the Project Settings window edits: the project's own settings,
// not the game module's defaults.
struct ProjectSettingsValues {
    PhysicsLayerTable layers{PhysicsLayerTable::defaults()};
    // The project's input action file: actions that replace the module's of
    // the same name, and actions of the project's own.
    InputActionMap inputActions;
    std::vector<std::string> tags;
};

// Reads the values from the project file and the input action file it names
// (none gives an empty map).
ProjectConfigResult loadProjectSettingsValues(const std::filesystem::path& projectFile, ProjectSettingsValues& values);
// Writes the values. The project file is read again first, so settings other
// windows saved since (the render settings) are kept. Input actions go to the
// file the project names, or to input_actions.json beside it, which the
// project then names; a project with no actions and no file stays without one.
ProjectConfigResult saveProjectSettingsValues(const std::filesystem::path& projectFile,
                                              const ProjectSettingsValues& values);
// Why the values cannot be saved, or empty.
std::string validateProjectSettings(const ProjectSettingsValues& values);

enum class ProjectSettingsTab : uint8_t {
    PhysicsLayers,
    InputActions,
    Tags,
};

// Edit > Project Settings: the project's physics layers and collision matrix,
// its input actions and bindings, and its tags. Edits change a draft; Apply
// returns it for the session to save and apply, and finish() reports how
// that went. Layers are only added and removed at the end, so the indices
// bodies store keep their meaning; the built-in Static, Dynamic, and Player
// layers cannot be renamed or removed.
class ProjectSettingsWindow {
  public:
    // moduleActions are the game module's defaults, offered to override.
    void open(ProjectSettingsValues values, InputActionMap moduleActions);
    bool isOpen() const;
    void close();

    // Draws the window while it is open. Returns the values to save on the
    // frame Apply is pressed; Apply is disabled unless canApply, with reason
    // shown.
    std::optional<ProjectSettingsValues> draw(bool canApply, const std::string& reason);
    // The window's contents, for tests.
    std::optional<ProjectSettingsValues> drawContents(bool canApply, const std::string& reason);
    // After Apply: an empty error marks the draft saved; otherwise it is shown.
    void finish(const std::string& error);

    ProjectSettingsValues& draft();
    bool modified() const;
    ProjectSettingsTab tab() const;
    void setTab(ProjectSettingsTab tab);

  private:
    void drawPhysicsLayers();
    void drawInputActions();
    void drawActionDetails(const std::string& action);
    void drawTags();

    bool m_open{false};
    ProjectSettingsTab m_tab{ProjectSettingsTab::PhysicsLayers};
    ProjectSettingsTab m_requestedTab{ProjectSettingsTab::PhysicsLayers};
    bool m_selectRequestedTab{false};
    ProjectSettingsValues m_saved;
    ProjectSettingsValues m_draft;
    InputActionMap m_moduleActions;
    std::string m_selectedAction;
    std::string m_newActionName;
    std::string m_newTag;
    std::string m_error;
    bool m_modified{false};
};
