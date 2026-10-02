#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/settings/EditorRenderSettings.hpp"

#include <string>
#include <vector>

class EditorDiagnostics;
class RenderRuntime;

// Edits the project's render settings, applies them live, and shows GPU pass
// timings. Owns the requested settings and where they are saved.
class GraphicsPanel final : public EditorPanel {
  public:
    explicit GraphicsPanel(EditorRenderSettings settings);

    const char* title() const override;
    void draw(EditorContext& context) override;

    // Loads the saved settings and applies them to the renderer.
    void loadSettings(RenderRuntime& rendering, EditorDiagnostics& diagnostics);

  private:
    void apply(RenderRuntime& rendering);

    EditorRenderSettings m_settings;
    // Capability fallbacks from the latest apply.
    std::vector<std::string> m_fallbacks;
};
