#pragma once

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/panels/EditorPanel.hpp"

#include <string>

// Shows and edits the active entity's name, transform, and components in one
// scrolling list, and offers whole-selection actions when several entities
// are selected. The sections come from EntityPropertiesView, which the
// Properties panel shares.
class InspectorPanel final : public EditorPanel {
  public:
    const char* title() const override;
    bool editModeOnly() const override;
    void draw(EditorContext& context) override;

  private:
    EntityPropertiesView m_view;
    std::string m_filter;
};
