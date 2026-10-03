#pragma once

#include "editor/menus/MenuRegistry.hpp"

#include <optional>
#include <string>

struct EditorContext;

// The entry a user chose: the operator to run and its JSON properties.
struct MenuChoice {
    std::string operatorId;
    std::string properties;
};

// Draws menu's items inside an open ImGui menu or popup, laid out as the menu
// asks: categories as submenus or headed sections, and leaves with their
// icon and bound shortcut, disabled when their operator's poll fails.
// Returns the leaf clicked this frame; the caller runs it.
std::optional<MenuChoice> drawMenuItems(EditorContext& context, const ResolvedMenu& menu);

// A menu shown as a popup at the mouse, Blender style: a search field takes
// keyboard focus when it opens; with an empty query the menu's items are
// shown, and typing lists the runnable leaves matching the query by label or
// path (for example "Light > Sun"). Up/Down move the highlight, Enter or a
// click chooses, Escape closes.
class MenuPopup {
  public:
    // Opens the popup at the next draw, which must use the same ImGui id stack.
    void open();
    // Closes the popup at the next draw.
    void close();
    bool isOpen() const;
    // Returns the entry chosen this frame; the popup then closes.
    std::optional<MenuChoice> draw(const char* popupId, EditorContext& context, const ResolvedMenu& menu);

  private:
    std::optional<MenuChoice> drawSearchResults(EditorContext& context, const ResolvedMenu& menu, bool entered,
                                                bool highlightMoved);

    std::string m_query;
    std::string m_previousQuery;
    int m_highlighted{0};
    bool m_openRequested{false};
    bool m_closeRequested{false};
    bool m_focusQuery{false};
    bool m_open{false};
};
