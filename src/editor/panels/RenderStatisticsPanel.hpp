#pragma once

// Headline renderer counters for the latest frame.

#include "editor/panels/EditorPanel.hpp"

// Headline renderer counters for the latest frame.
class RenderStatisticsPanel final : public EditorPanel {
  public:
    const char* title() const override;
    void draw(EditorContext& context) override;
};
