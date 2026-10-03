#pragma once

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/panels/EditorPanel.hpp"

#include <string>

// Shows and edits the selection in one scrolling list: the active entity's
// name, transform, and components, or, when several entities are selected,
// the whole-selection actions and the components they share (edited together)
// followed by those only the active entity has. The sections come from
// EntityPropertiesView, which the Properties panel shares.
class InspectorPanel final : public EditorPanel {
  public:
    // The layout is shared by every Inspector and Properties area and must
    // outlive the panel.
    explicit InspectorPanel(ComponentInspectorLayout& layout);

    const char* title() const override;
    bool editModeOnly() const override;
    void draw(EditorContext& context) override;

  private:
    EntityPropertiesView m_view;
    std::string m_filter;
};
