#pragma once

#include "engine/graphics/Rendering.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct EditorContext;
class ViewportPanel;

// Identifies one viewport panel for as long as it exists; never reused.
struct ViewportId {
    uint32_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    bool operator==(const ViewportId&) const = default;
};

// A sidebar tab contributed from outside the viewports, for example by a game
// module, drawn after the built-in Item, Tool, and View tabs of every
// viewport. Whoever adds it must remove it before the code draw runs is
// unloaded.
struct ViewportSidebarTab {
    // Tab label; unique among the viewport's tabs.
    std::string title;
    std::function<void(EditorContext&)> draw;
};

// Titles of the built-in sidebar tabs, which added tabs may not reuse.
namespace BuiltInViewportSidebarTab {
inline constexpr const char* item = "Item";
inline constexpr const char* tool = "Tool";
inline constexpr const char* view = "View";
} // namespace BuiltInViewportSidebarTab

struct ViewportSidebarTabResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The session's record of its viewport panels, shared by every viewport area:
// which viewports exist and are open, which one is active, the render views
// they hold, and the sidebar tabs added for all of them.
//
// Open and active: a viewport is open once drawn in the current frame and
// until a beginFrame finds it was not drawn since the previous one; before
// the first beginFrame every viewport counts as open. The active viewport is the one last
// activated (hovered or clicked) while it is open, otherwise the first open
// viewport, otherwise none. Operators and the status bar act on it, so every
// caller must handle there being none.
//
// Render views: a viewport asking while the renderer's default view is free
// gets it (it keeps GPU-driven submission and renders every frame once
// sized); others get views of their own. A viewport returns its view when it
// is destroyed. The default view must not idle in a viewport nobody sees, so
// the session makes a closed viewport release it (ViewportPanel::
// releaseRenderView), and the next drawn viewport trades its own view for it.
//
// The registry must outlive every viewport panel registered with it, and the
// RenderRuntime a view came from must outlive the view's release.
class ViewportRegistry {
  public:
    ViewportRegistry() = default;
    ViewportRegistry(const ViewportRegistry&) = delete;
    ViewportRegistry& operator=(const ViewportRegistry&) = delete;

    // Called by ViewportPanel's constructor and destructor.
    ViewportId add(ViewportPanel& panel);
    void remove(ViewportId id);

    // Null for an unknown or removed id.
    ViewportPanel* find(ViewportId id) const;
    size_t size() const;

    // Starts a frame: viewports drawn since the previous call are open.
    void beginFrame();
    // Records that the viewport was drawn this frame.
    void markDrawn(ViewportId id);
    bool open(ViewportId id) const;
    // Open viewports in creation order.
    std::vector<ViewportPanel*> openViewports() const;

    // Makes the viewport active; unknown ids are ignored.
    void activate(ViewportId id);
    ViewportPanel* active() const;

    // A render view for a viewport: the default view while no viewport holds
    // it, otherwise a created view. Empty when the renderer has no free view.
    RenderViewId acquireView(RenderRuntime& rendering, ViewportId requester);
    // Destroys a created view; the default view has its per-view state
    // (camera, grid, outlines, settings, input) reset and becomes free.
    void releaseView(RenderRuntime& rendering, RenderViewId view);
    bool defaultViewFree() const;
    // The viewport holding the default view, or null.
    ViewportPanel* defaultViewHolder() const;

    // Rejects an empty title, a missing draw function, and a title already
    // used by a built-in or added tab.
    ViewportSidebarTabResult addSidebarTab(ViewportSidebarTab tab);
    // Returns whether a tab with title was removed.
    bool removeSidebarTab(std::string_view title);
    std::span<const ViewportSidebarTab> sidebarTabs() const;

  private:
    struct Entry {
        ViewportId id;
        ViewportPanel* panel{nullptr};
        bool open{true};
        bool drawnThisFrame{false};
    };

    const Entry* entry(ViewportId id) const;
    Entry* entry(ViewportId id);

    std::vector<Entry> m_entries;
    uint32_t m_nextId{1};
    ViewportId m_active;
    ViewportId m_defaultViewHolder;
    std::vector<ViewportSidebarTab> m_sidebarTabs;
};
