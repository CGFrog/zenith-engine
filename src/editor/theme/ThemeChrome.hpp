#pragma once

#include "editor/theme/GlassBackdrop.hpp"
#include "editor/theme/ThemeParameters.hpp"

#include <imgui.h>

struct ImGuiContext;

// Window-level painting for the pixel theme: the wallpaper behind the
// dockspace, glass frames behind docked areas and the editor's bars, and
// bevels on popups. The editor session owns one and calls it while drawing.
class ThemeChrome {
  public:
    // The theme tuning used from the next beginFrame on (the defaults until
    // set).
    void setParameters(const ThemeParameters& parameters);

    const ThemeParameters& parameters() const {
        return m_parameters;
    }

    // Once per frame before the dockspace: keeps the wallpaper current and
    // paints it behind every window.
    void beginFrame(RenderRuntime& rendering);
    void release(RenderRuntime& rendering);

    // Call right after an area window's Begin succeeds; the window must have
    // been begun with a transparent WindowBg. A docked area becomes a glass
    // frame around an inset solid pane, painted behind its dock node; a
    // floating one gets the solid pane below its title bar.
    void paintAreaWindow() const;
    // Call right after a bar window's Begin (the main menu bar, the status
    // bar), begun with a transparent WindowBg: glass across the whole window.
    void paintBarWindow() const;
    // Smoked glass for chrome floating over the 3D viewport, which has no
    // wallpaper behind it to frost.
    static void paintSmokedGlass(ImDrawList* drawList, ImVec2 min, ImVec2 max);
    // Adds a pixel bevel to every popup, menu, and tooltip at the end of each
    // frame. Installing twice on one context has no further effect.
    static void installPopupBevels(ImGuiContext& context);

    const GlassBackdrop& backdrop() const {
        return m_backdrop;
    }

  private:
    void paintGlassPanel(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;

    GlassBackdrop m_backdrop;
    ThemeParameters m_parameters;
};
