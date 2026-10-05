#pragma once

#include "editor/project/BuildRunner.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class EditorDiagnostics;
class FastProjectBuild;
class FileWatcher;
struct EditorToolSettings;

// The editor's one build: what to run (the open project's build, or the
// build.command and build.directory settings), the runner that runs it, and
// the "Build" diagnostics source it publishes. The session owns it and polls
// it every frame, so builds started with Ctrl B progress with no Build area
// open; the Build panel shows it and the build operators start and cancel it.
class ProjectBuild {
  public:
    // tools and diagnostics must outlive the build.
    ProjectBuild(const EditorToolSettings& tools, EditorDiagnostics& diagnostics);
    ~ProjectBuild();
    ProjectBuild(const ProjectBuild&) = delete;
    ProjectBuild& operator=(const ProjectBuild&) = delete;

    // Starts the build request. A start failure replaces the "Build"
    // diagnostics with one error; a started build clears them.
    ChildProcessResult start();
    // Starts a build now, or, while one runs, queues one more that starts
    // when it finishes (several requests queue one build).
    void request();
    // Cancels the running build and any queued one.
    void cancel();
    // Moves the build's output along and publishes its diagnostics when it
    // finishes; then starts a queued build. Also watches the sources while
    // build on save is on (see watchSources).
    void poll();

    const BuildRunner& runner() const;
    bool running() const;
    bool queued() const;
    // Counts started builds, so a view can tell a new one began.
    uint64_t startCount() const;

    // The open project's own build (see projectBuildRequest), run instead of
    // the settings while build.command and build.directory are unchanged
    // from their defaults; editing either builds with the settings again.
    void setProjectBuild(std::optional<BuildRequest> projectBuild);
    // The project's fast build (a Ninja tree with the engine's compiler),
    // used in place of the project build whenever it can run. While its
    // developer environment is still being captured, a started build waits
    // (queued) rather than taking the slower path.
    void setFastBuild(std::unique_ptr<FastProjectBuild> fastBuild);
    // The request start() runs now.
    BuildRequest buildRequest() const;
    bool usesProjectBuild() const;
    // The source directory build on save watches: the project's src folder,
    // only while the project's own build is used. Empty otherwise.
    std::filesystem::path watchedSourceDirectory() const;

  private:
    // True while the project build waits for the fast build's environment.
    bool waitingForFastBuild() const;
    void publishDiagnostics() const;
    // Keeps the watcher matching build.on_save and the source directory, and
    // requests a build when a source file changed.
    void watchSources();

    const EditorToolSettings& m_tools;
    EditorDiagnostics& m_diagnostics;
    std::optional<BuildRequest> m_projectBuild;
    std::unique_ptr<FastProjectBuild> m_fastBuild;
    bool m_reportedFastBuild{false};
    BuildRunner m_runner;
    uint64_t m_startCount{0};
    bool m_queued{false};
    std::unique_ptr<FileWatcher> m_watcher;
    std::filesystem::path m_watchedDirectory;
    // The last watcher start failure, reported once until it changes.
    std::string m_watcherError;
};

// Whether a change to path (relative to the watched source directory) should
// start a build on save: C and C++ sources and headers, and CMake lists.
bool buildOnSaveWatches(const std::filesystem::path& path);
