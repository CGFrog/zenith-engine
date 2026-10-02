#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "engine/scene/Scene.hpp"

#include <optional>
#include <string>

// The scene outliner: a filterable entity tree with creation, deletion,
// duplication, and drag-and-drop reparenting.
class HierarchyPanel final : public EditorPanel {
  public:
    const char* title() const override;
    bool editModeOnly() const override;
    void draw(EditorContext& context) override;

  private:
    void drawEntity(EditorContext& context, Entity entity);
    void drawContextMenu(EditorContext& context, Entity entity);
    // Accepts a dragged entity and reparents it under parent (null for root).
    void acceptReparentDrop(EditorContext& context, Entity parent) const;
    bool matchesFilter(const std::string& name) const;

    std::string m_filter;
    bool m_preserveWorldOnReparent{true};
    // Expand All / Collapse All request opening or closing every tree node;
    // the request applies for the whole of the next frame.
    std::optional<bool> m_requestedAllOpen;
    std::optional<bool> m_setAllOpen;
};
