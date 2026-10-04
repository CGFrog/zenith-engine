#pragma once

#include "editor/application/EditorAppearance.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/project/ProjectHub.hpp"
#include "editor/project/RecentProjects.hpp"
#include "engine/application/Application.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <filesystem>
#include <memory>
#include <optional>

// The Project Hub's configuration, for an editor that offers it.
struct EditorProjectHubOptions {
    // The hub's template, engine, and default folder (its ideCommand is read
    // from the editor settings whenever the hub opens).
    ProjectHubConfig hub;
    // Start in the hub instead of a session for the constructor's project.
    bool startInHub{false};
};

// The editor executable's application. It owns what outlives one project:
// the ImGui theme and fonts (an EditorAppearance created in onStart in the
// look settings.ini chooses; the hub draws in it and sessions apply the
// preferences' live changes through it), the recent projects
// list (recent-projects.json in the settings directory, updated whenever a
// project file is opened), and the current project. It shows either the
// Project Hub or one EditorSession for the current project.
//
// Switching projects goes through the hub and replaces the session object:
// File > Project Hub (or Open Project...) asks the session about unsaved
// changes and requests the hub scene, which unloads the session (saving its
// workspaces and settings and unloading its game module); choosing a project
// in the hub loads its file and requests a new session, so nothing of one
// project's session state carries over to the next.
class EditorApplication final : public Application {
  public:
    // Without projectHub the editor opens project directly and File > Project
    // Hub is unavailable (automated runs and tests).
    // play is what the Standalone play target launches (the game executable
    // and, in tests, a fake process).
    EditorApplication(std::filesystem::path settingsDirectory, std::filesystem::path fontPath, ProjectConfig project,
                      std::optional<EditorProjectHubOptions> projectHub = std::nullopt, EditorPlayServices play = {});

    SceneResult onStart(ApplicationContext& context) override;
    // The session's policy (EditorSession::mouseCapturePolicy), which it
    // publishes each frame: Persistent while a game view holds game input.
    MouseCapturePolicy mouseCapturePolicy() const override;
    SceneKey initialScene() const override;
    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override;

    // Loads projectFile, records it in the recent projects, and switches to
    // a session for it at the end of the frame.
    ProjectConfigResult openProject(ApplicationContext& context, const std::filesystem::path& projectFile);
    // Ends the session and shows the hub at the end of the frame.
    void showProjectHub(ApplicationContext& context, ProjectHubEntry entry);

    const ProjectConfig& project() const;
    const RecentProjects& recentProjects() const;

  private:
    void recordRecentProject(Logger& logger);
    std::unique_ptr<ApplicationScene> createSession();
    std::unique_ptr<ApplicationScene> createProjectHub();

    std::filesystem::path m_settingsDirectory;
    std::filesystem::path m_fontPath;
    // The look on the ImGui context and its font cache, created by onStart
    // from settings.ini and shared with the hub and every session, which
    // apply their preference changes through it.
    std::unique_ptr<EditorAppearance> m_appearance;
    ProjectConfig m_project;
    std::optional<EditorProjectHubOptions> m_projectHub;
    EditorPlayServices m_play;
    // Written by the current session (EditorProjectServices::
    // mouseCaptureChanged).
    MouseCapturePolicy m_mouseCapturePolicy{MouseCapturePolicy::WhileRightMouseDown};
    RecentProjects m_recentProjects;
    ProjectHubEntry m_hubEntry{ProjectHubEntry::Hub};
};
