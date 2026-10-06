#pragma once

// The Project Hub: recent projects, new from template, open project.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/FolderPicker.hpp"
#include "editor/application/NativeFileDialog.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/RecentProjects.hpp"
#include "engine/platform/ChildProcess.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;
class ThemeChrome;

// What the hub shows first when the editor leaves a project for it: its
// lists, or the native dialog for choosing a project file.
enum class ProjectHubEntry {
    Hub,
    OpenProjectDialog,
};

// Where the hub finds what New from Template needs, and the IDE command.
struct ProjectHubConfig {
    // The templates New from Template offers (listProjectTemplates), the
    // first selected; empty disables New from Template.
    std::vector<ProjectTemplateInfo> templates;
    // The Zenith build new projects compile their game module against.
    std::filesystem::path engineRoot;
    // The engine's assets, the source of the template's starter model.
    std::filesystem::path engineAssetRoot;
    // The configuration new projects build their module in (the editor's).
    std::string buildConfiguration{"Debug"};
    // The folder New from Template suggests.
    std::filesystem::path defaultProjectLocation;
    // The editor's settings.ini. Open in IDE reads its ide.command (see
    // expandExternalEditorCommand) when clicked, so a command changed in a
    // session that just closed applies. Empty uses the default command.
    std::filesystem::path editorSettingsFile;
};

// What Open in IDE opens for a project: its folder, which holds the project's
// CMakeLists.txt, at line 1.
EditorSourceLocation projectIdeLocation(const std::filesystem::path& projectFile);
// Opens the project's folder with the IDE command, detached from the editor.
ChildProcessResult openProjectInIde(std::string_view ideCommand, const std::filesystem::path& projectFile);

// The editor's start screen, drawn over the whole window before a project is
// open: the recent projects (name, path, and when each was last opened; a
// missing one is flagged and can be removed), New from Template, Open
// existing (a native dialog for .zenith-project files), and Open in IDE. It
// only chooses a project; the editor application opens it and records it in
// the recent projects.
class ProjectHub {
  public:
    // recentProjects must outlive the hub; the hub saves it when the user
    // removes an entry.
    ProjectHub(RecentProjects& recentProjects, ProjectHubConfig config);

    // Draws one frame over the main viewport, on chrome's pane when given.
    // Returns the project file the user opened or created this frame.
    std::optional<std::filesystem::path> draw(AssetManager& assets, const ThemeChrome* chrome = nullptr);

    // Shows the native open dialog, as Open Project... does.
    void browseForProject();
    // Shows why the chosen project could not be opened.
    void reportError(std::string message);

    // The last status or error line the hub shows.
    const std::string& message() const;
    bool messageIsError() const;
    const ProjectHubConfig& config() const;

  private:
    void drawHeader() const;
    void drawRecentProjects();
    void drawRecentProjectRow(const RecentProject& project);
    void drawNewProject(AssetManager& assets);
    void drawOpenExisting();
    void drawMessage() const;
    void pollOpenDialog();
    void setMessage(std::string message, bool isError);

    RecentProjects& m_recentProjects;
    ProjectHubConfig m_config;
    NativeFileDialog m_openDialog;
    std::string m_newProjectName{"My Game"};
    // Index into the config's templates.
    size_t m_selectedTemplate{0};
    std::string m_newProjectLocation;
    FolderPicker m_locationPicker;
    std::string m_message;
    bool m_messageIsError{false};
    // Set by a button or the dialog; returned and cleared by draw().
    std::optional<std::filesystem::path> m_chosen;
    // Removing a row while the list is drawn is deferred to the list's end.
    std::optional<std::filesystem::path> m_pendingRemoval;
};
