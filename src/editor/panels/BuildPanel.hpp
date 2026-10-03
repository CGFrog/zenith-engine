#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/project/BuildRunner.hpp"

#include <deque>
#include <optional>
#include <string>

class EditorDiagnostics;
class PackageRunner;
struct EditorToolSettings;

// Runs the configured build command in the background and streams its output.
// Compiler errors are highlighted, open in the IDE when clicked, and replace
// the previous build's entries (source "Build") in the editor diagnostics.
// It also shows the output of Build and Package (file.build_package), whose
// runner the session owns and polls.
class BuildPanel final : public EditorPanel {
  public:
    // Which run's output the panel shows.
    enum class View : uint8_t {
        Build,
        Package,
    };

    // The panel edits the build and IDE settings in place; tools (and
    // package, when given) must outlive it.
    explicit BuildPanel(EditorToolSettings& tools, PackageRunner* package = nullptr);

    const char* title() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;

    // The session-independent halves of update() and draw(), for tests.
    void startBuild(EditorDiagnostics& diagnostics);
    void poll(EditorDiagnostics& diagnostics);
    void drawContents(EditorDiagnostics& diagnostics);
    const BuildRunner& runner() const;

    // The open project's own build (see projectBuildRequest), run instead of
    // the settings while build.command and build.directory are unchanged
    // from their defaults; editing either builds with the settings again.
    void setProjectBuild(std::optional<BuildRequest> projectBuild);
    // The request the Build button runs now.
    BuildRequest buildRequest() const;
    bool usesProjectBuild() const;

    View view() const {
        return m_view;
    }

  private:
    void publishDiagnostics(EditorDiagnostics& diagnostics) const;
    // Shows a package's output when one starts.
    void followPackage();
    void drawToolbar(EditorDiagnostics& diagnostics);
    void drawStatus() const;
    void drawPackageStatus() const;
    void drawOutput(const std::deque<BuildOutputLine>& output, EditorDiagnostics& diagnostics);

    EditorToolSettings& m_tools;
    std::optional<BuildRequest> m_projectBuild;
    BuildRunner m_runner;
    PackageRunner* m_package{nullptr};
    View m_view{View::Build};
    bool m_packageWasRunning{false};
    bool m_followOutput{true};
    bool m_scrollToEnd{false};
};
