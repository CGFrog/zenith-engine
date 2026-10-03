#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/project/BuildRunner.hpp"

#include <optional>
#include <string>

class EditorDiagnostics;
struct EditorToolSettings;

// Runs the configured build command in the background and streams its output.
// Compiler errors are highlighted, open in the IDE when clicked, and replace
// the previous build's entries (source "Build") in the editor diagnostics.
class BuildPanel final : public EditorPanel {
  public:
    // The panel edits the build and IDE settings in place; tools must outlive
    // it.
    explicit BuildPanel(EditorToolSettings& tools);

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

  private:
    void publishDiagnostics(EditorDiagnostics& diagnostics) const;
    void drawToolbar(EditorDiagnostics& diagnostics);
    void drawStatus() const;
    void drawOutput(EditorDiagnostics& diagnostics);

    EditorToolSettings& m_tools;
    std::optional<BuildRequest> m_projectBuild;
    BuildRunner m_runner;
    bool m_followOutput{true};
    bool m_scrollToEnd{false};
};
