#pragma once

#include "editor/panels/EditorPanel.hpp"

class RenderSettingsController;

// Edits the project's render settings, applies them live, and shows GPU pass
// timings. The requested settings and where they are saved belong to the
// session's RenderSettingsController, which the Properties panel shares.
class GraphicsPanel final : public EditorPanel {
  public:
    // The controller must outlive the panel.
    explicit GraphicsPanel(RenderSettingsController& settings);

    const char* title() const override;
    void draw(EditorContext& context) override;

  private:
    RenderSettingsController& m_settings;
};
