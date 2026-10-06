#pragma once

#include "editor/application/FolderPicker.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/project/BuildRunner.hpp"

#include <cstdint>
#include <deque>
#include <string>

class EditorDiagnostics;
class PackageRunner;
class ProjectBuild;
struct EditorToolSettings;

// Shows the session's build (ProjectBuild) as it streams: compiler errors are
// highlighted and open in the IDE when clicked. Its Build and Cancel buttons
// run build.run and build.cancel. It also shows the output of Build and
// Package (file.build_package), whose runner the session owns and polls.
class BuildPanel final : public EditorPanel {
  public:
    // Which run's output the panel shows.
    enum class View : uint8_t {
        Build,
        Package,
    };

    // The panel edits the build and IDE settings in place; tools, build (and
    // package, when given) must outlive it.
    BuildPanel(EditorToolSettings& tools, const ProjectBuild& build, PackageRunner* package = nullptr);

    const char* title() const override;
    void draw(EditorContext& context) override;
    // What draw() draws, inside the caller's window; for tests.
    void drawContents(EditorContext& context);

    View view() const {
        return m_view;
    }

  private:
    // Shows a build's or a package's output when one starts.
    void followRuns();
    void drawToolbar(EditorContext& context);
    void drawStatus() const;
    void drawPackageStatus() const;
    void drawOutput(const std::deque<BuildOutputLine>& output, EditorDiagnostics& diagnostics);

    EditorToolSettings& m_tools;
    FolderPicker m_directoryPicker;
    const ProjectBuild& m_build;
    PackageRunner* m_package{nullptr};
    View m_view{View::Build};
    uint64_t m_seenBuildStarts{0};
    bool m_packageWasRunning{false};
    bool m_followOutput{true};
    bool m_scrollToEnd{false};
};
