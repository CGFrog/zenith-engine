#pragma once

#include "editor/theme/ThemeParameters.hpp"

#include <imgui.h>

struct ImGuiContext;

// Window-level painting for the pixel theme: the dark gap behind the
// dockspace, square areas with a header strip over a solid pane, header bars,
// and bevels on popups. The editor session owns one and calls it while
// drawing.
class ThemeChrome {
  public:
    // The theme tuning used from the next beginFrame on (the defaults until
    // set).
    void setParameters(const ThemeParameters& parameters);

    const ThemeParameters& parameters() const {
        return m_parameters;
    }

    // The wash painted over the gap, headers, and panes, and around the edge
    // of the main viewport from the next beginFrame on (none until set). The
    // 3D views' images stay untinted.
    void setModeTint(ThemeModeTint tint);

    ThemeModeTint modeTint() const {
        return m_modeTint;
    }

    // Once per frame before the dockspace: paints the gap behind every
    // window.
    void beginFrame() const;

    // Call right after an area window's Begin succeeds; the window must have
    // been begun with a transparent WindowBg. A docked area becomes a square
    // header strip (behind its dock node's tab bar) over a solid pane; a
    // floating one gets the pane below its title bar.
    void paintAreaWindow() const;
    // Call right after a bar window's Begin (the main menu bar, the status
    // bar), begun with a transparent WindowBg: a header strip across the
    // whole window.
    void paintBarWindow() const;
    // A translucent header for chrome floating over the 3D viewport.
    static void paintOverlayPanel(ImDrawList* drawList, ImVec2 min, ImVec2 max);
    // Adds a pixel bevel to every popup, menu, and tooltip at the end of each
    // frame. Installing twice on one context has no further effect.
    static void installPopupBevels(ImGuiContext& context);

  private:
    void paintHeader(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;
    void paintPane(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;
    // The colour with the mode tint composited in, keeping its opacity.
    ThemeColor tinted(ThemeColor color) const;

    ThemeParameters m_parameters;
    ThemeModeTint m_modeTint{ThemeModeTint::None};
};
